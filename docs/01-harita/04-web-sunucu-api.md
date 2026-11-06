# 04 — HTTP Sunucusu ve API Yüzeyi (ALTE/HW)

Kaynak: `ALTE/HW/components/web_server/web_server.c` (2655 satır, tamamı okundu) ve `include/web_server.h` (21 satır). Aşağıda yalın `web_server.c:N` biçimindeki satır referansları bu dosyaya aittir; diğer dosyalar tam yoluyla verilir. ESP-IDF 5.5.2 varsayılanları `~/esp/esp-idf-v5.5.2/components/esp_http_server/` altından doğrulanmıştır.

Dışa açık API yalnızca üç fonksiyondur (`web_server.h:10-21`): `web_server_start()`, `web_server_stop()`, `web_server_send_timer_event()`.

---

## 1. Sunucu kurulumu

### 1.1 httpd yapılandırması (`web_server.c:2603-2643`)

| Parametre | Değer | Kaynak |
|---|---|---|
| Port | 80 (HTTP, TLS yok) | `HTTPD_DEFAULT_CONFIG()` → `.server_port = 80`; `web_server.c:2610` |
| Kontrol portu | 32768 (`ESP_HTTPD_DEF_CTRL_PORT`) | IDF varsayılanı |
| Görev yığını | **8192** bayt (varsayılan 4096) | `web_server.c:2613` |
| Görev önceliği / çekirdek | `tskIDLE_PRIORITY+5`, `tskNO_AFFINITY` | IDF varsayılanı |
| `max_open_sockets` | 7 | `web_server.c:2614` (yorum: "LWIP varsayılan max"; gerçekte `CONFIG_LWIP_MAX_SOCKETS=16`, `sdkconfig.defaults:27`) |
| `lru_purge_enable` | **true** — soket dolunca en eski boşta bağlantı kapatılır | `web_server.c:2615` |
| `max_uri_handlers` | rota sayısı (55) + 4 = **59** | `web_server.c:2612` |
| `uri_match_fn` | `httpd_uri_match_wildcard` (yalnızca `/ext/*` için gerekli) | `web_server.c:2611` |
| `close_fn` | `sse_session_close` — her soket kapanışında SSE slotunu boşaltır ve `close(sockfd)` çağırır | `web_server.c:2616`, `1376-1386` |
| `recv_wait_timeout` / `send_wait_timeout` | 5 s / 5 s | IDF varsayılanı (değiştirilmemiş) |
| `backlog_conn` | 5 | IDF varsayılanı |
| `max_resp_headers` | 8 | IDF varsayılanı |
| `keep_alive_enable` | false (TCP keep-alive yok; HTTP/1.1 kalıcı bağlantı httpd tarafından yine de desteklenir) | IDF varsayılanı |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` | 1024 | `ALTE/HW/sdkconfig.defaults:23` |
| `CONFIG_HTTPD_MAX_URI_LEN` | 512 | `ALTE/HW/sdkconfig.defaults:24` |
| WebSocket | kapalı (`CONFIG_HTTPD_WS_SUPPORT` varsayılan n; sdkconfig.defaults'ta açılmamış) | Kconfig |
| Hata yanıtı gecikmesi | `CONFIG_HTTPD_ERR_RESP_NO_DELAY=y` (varsayılan) | Kconfig |

Önemli mimari gerçek: ESP-IDF httpd **tek görevli**dir. Tüm handler'lar sırayla aynı görevde (8 KB yığın) çalışır; bir handler bloklanırsa (ör. SMTP testi) sunucunun tamamı o süre boyunca yanıt vermez. Bu, §6 ve §8'deki bulguların temelidir.

### 1.2 Başlatma / durdurma

- `web_server_start()` (`web_server.c:2603-2643`): `httpd_start` → `auth_load()` (NVS'den parola/oturum ayarları, `:2623`) → 55 rotanın tamamı `route_dispatch` sarmalayıcısıyla kaydedilir (`:2625-2634`, `user_ctx` = rota satırı) → `sse_send` görevi yaratılır (3072 bayt yığın, öncelik 5; `:2637-2639`). Tekrar çağrılırsa "Server zaten calisiyor" uyarısıyla `ESP_OK` döner (`:2605-2608`).
- Çağıran: `ALTE/HW/main/main.c:494` — `wifi_manager_init()` (AP başlatma, `main.c:487`) sonrasında, `wifi_manager_auto_connect()` (`main.c:527`) **öncesinde**; yorum: "AP uzerinden erisim icin auto_connect'ten ONCE hazir olmali".
- `web_server_stop()` (`:2645-2655`): `httpd_stop` + `s_server = NULL`. SSE görevi silinmez, `s_sse_fds` sıfırlanmaz. **Hiçbir yerden çağrılmıyor** (grep: yalnızca tanım). Ölü API.
- `web_server_send_timer_event()` (`:1435-1440`): `s_sse_task_handle`'a `xTaskNotifyGive`; çağıranlar: `main.c:99` (buton), `main.c:355` (süre doldu), `main.c:477` (`timer_manager_set_tick_callback` — periyodik tik), ayrıca `web_server.c` içinden timer/set ve `/api/reset`.

### 1.3 AP ve STA farkı, captive portal

- Fark **yok**: tek `httpd` örneği tüm arabirimlerde (AP 192.168.4.1 ve STA IP'si) aynı rotaları sunar. `/api/system/info` AP IP'sini sabit `"192.168.4.1"` olarak döner (`:704`).
- Captive portal / DNS yakalama / HTTP yönlendirme **yok** (`grep -i "captive|dns_server|redirect|Location:"` → yalnızca OTA/HTTP istemcisinin `max_redirection_count`'u). Kullanıcı tarayıcıya `http://192.168.4.1` veya mDNS adını kendisi yazmalı.
- mDNS: `wifi_manager.c:408-423` hostname = küçük harfli cihaz adı, `_http._tcp` port 80 servisi duyurulur. Sunucu bu konuda bir şey yapmaz.
- AP **parolasız** açılır (`wifi_manager.h:26`: "AP modda baslar, sifresiz"). Bu, §8'de HTTP-düz-metin bulgusunu ağırlaştırır.

---

## 2. Kimlik doğrulama

Tasarım özeti yorum bloğunda (`web_server.c:118-129`). Uygulama:

### 2.1 Parola saklama
- NVS ad alanı `ls_auth`, anahtar `hash`: `hex(SHA-256("<mac6hex>:<parola>"))`, tuz = eFuse MAC (`auth_hash_password`, `:161-172`). Tek tur SHA-256, KDF yok (§8).
- Yüklenme: `auth_load()` (`:174-196`) — `hash` 64 hex ise `s_auth_enabled = true`; `ttl_min` (1-1440, varsayılan 10), `protect` (varsayılan 1), `lock_min` (1-60, varsayılan 15). Yüklemede tüm oturumlar sıfırlanır (`:192`).
- Yazma: `auth_store_password()` (`:198-216`) — parola boş/NULL → `hash` silinir, kimlik doğrulama kapanır. Her değişiklikte **tüm oturumlar düşer** (`:214`). `auth_store_session_cfg()` (`:218-229`) ttl/protect/lock yazar.
- Parola yoksa her şey açıktır ("GUI uyarı gösterir", `:122`).

### 2.2 Oturum (çerez)
- Belirteç: 16 rastgele bayt (`esp_fill_random`) → 32 hex karakter (`auth_new_token`, `:231-237`).
- Saklama: **yalnızca RAM**, `s_sessions[4]` (`AUTH_MAX_SESSIONS=4`, `:131`); doluysa en eski `last_seen_s` olan ezilir (`auth_session_create`, `:239-250`). Yeniden başlatmada tüm oturumlar gider.
- Çerez: `ls_sess=<tok>; Path=/; Max-Age=<ttl_min*60>; HttpOnly; SameSite=Strict` (`auth_set_cookie`, `:305-311`). `Secure` yok (TLS de yok).
- Süre: **hareketsizlik** bazlı; her başarılı doğrulamada `last_seen_s` güncellenir (`:298`), `now - last_seen > ttl` ise oturum silinir (`:296`). Saat kaynağı `esp_timer` (uptime, `:151`).
- İstek doğrulama (`auth_request_ok`, `:280-303`): önce `X-Auth-Token` başlığı (`:284`), yoksa `Cookie` başlığında `ls_sess=` (başlangıçta veya `; ` sonrasında eşleşir; başlık ≤ 4096 bayt, dinamik tampon; `auth_cookie_token`, `:255-277`). Karşılaştırma sabit zamanlı `ct_equal` (`:154-159`).
- `logout` yalnızca **çerezi** okur, `X-Auth-Token` başlığını okumaz (`:372`) — tutarsızlık.

### 2.3 Başarısız deneme kilidi
- `AUTH_FAIL_MAX=5` (`:133`). `s_auth_fail` **küresel** sayaç (IP'ye göre değil). `protect` açıkken 5 hata → `s_auth_lock_until = now + lock_min*60`, sayaç sıfırlanır (`:349-355`). Kilitliyken login `429 Too Many Requests` + `{"status":"locked","retry_s":N}` (`:332-338`). `protect` kapalıysa sınırsız deneme.
- Her hatalı giriş ve kilit `sys_log_write("WARN","user",...)` ile kalıcı loga yazılır (`:350,354`).

### 2.4 Korumalı rotalar
Rota tablosundaki `auth` bayrağı (`:2541-2597`) `route_dispatch` (`:2529-2538`) tarafından uygulanır: `r->auth && s_auth_enabled && !auth_request_ok(req)` → `401` + `Cache-Control: no-store` + `{"status":"error","error":"auth_required"}`. Gövde okunmadan reddedilir (httpd kalan gövdeyi `httpd_req_delete` içinde 32 baytlık tamponla boşaltır — büyük upload'da bant genişliği harcanır).

`auth=false` olan 7 rota: `/`, `/api/auth/status`, `/api/auth/login`, `/api/auth/set` (handler kendisi doğrular, `:395`), `/api/reset` (kendi anahtarı), `/ext/*`. Diğer 49 rota korumalı. Statik GUI dosyaları açıktır (GUI kabuğu yüklenir, API 401 verince giriş ekranı gösterilir).

### 2.5 API anahtarı (`/api/reset?key=`)
- Üretim: `esp_random()` 32 bit → `"ls_%08lx"` (11 karakter, 8 hex) (`:1990-1992`); NVS `ls_reset/api_key`, üretince `enabled=1` otomatik (`:2000-2002`).
- Doğrulama (`:2012-2058`): `enabled` yoksa 403; uzunluk eşit + XOR-toplam sabit zamanlı karşılaştırma (`:2038-2043`); eşleşmezse 401. Hız sınırı / kilit **yok**; `s_auth_fail` ile ilişkisiz.
- Maskeleme (`/api/reset/config` GET, `:1935-1943`): son 4 karakter `****` → `ls_a3f7****` örneğinde 8 hex'in 4'ü açıkta kalır (kalan entropi 16 bit).

### 2.6 CSRF / CORS
- CSRF savunması yalnızca `SameSite=Strict` çerezidir (`:126`). Origin/Referer/Host denetimi, Content-Type zorunluluğu, CSRF belirteci **yok**. Parola **ayarlı değilken hiçbir CSRF koruması yoktur** (§8.1).
- CORS başlığı yalnızca SSE'de: `Access-Control-Allow-Origin: *` (`:1359`). OPTIONS/preflight handler'ı yok; diğer API yanıtlarında CORS başlığı yok.
- `/api/reset` GET'i tasarım gereği çapraz kaynak/yetkisiz erişime açıktır.

---

## 3. Statik dosya sunumu

### 3.1 Rotalar
- `GET /` → `root_handler` (`:680-683`) → `serve_static_path(req, "/index.html")`.
- `GET /ext/*` → `ext_file_handler` (`:1443-1456`): `/ext` öneki atılır, boş/`/` → `/index.html`, `clean_path[128]`'e kopyalanır (uzun yol sessizce **kesilir**), `?` sonrası (`?v=0.0.70`) atılır.
- `/index.html`, `/style.css`, `/lang/en.json` gibi öneksiz yollar **404** (yalnızca `/` ve `/ext/...`). GUI `index.html` içinde `/ext/app.js?v=0.0.70`, `/ext/style.css?v=...`, `/ext/pic/logo.png` kullanır.

### 3.2 Beyaz liste (`static_path_allowed`, `:617-626`)
- Reddedilir: `/` ile başlamayan, `..` içeren, `\` içeren.
- Tam eşleşme: `/index.html`, `/style.css`, `/app.js`, `/config.json`, `/favicon.ico` (favicon gömülü değil → slot'ta yoksa 404).
- Önek: `/pic/<tek seviye>`, `/lang/<tek seviye>` (`strchr(p+1,'/') == strrchr(p,'/')`).
- httpd URI'yi yüzde-çözmez; `%2e%2e` LittleFS'te harfiyen aranır, traversal olmaz.

### 3.3 Kaynak sırası (`serve_static_path`, `:641-677`)
1. Slot A veya B monte ise (`ext_flash_is_slot_a_mounted() || ext_flash_is_slot_b_mounted()`):
   a. istemci `Accept-Encoding` içinde `gzip` geçiriyorsa (`client_accepts_gzip`, `:628-633`, başlık ≤127 bayt) ve aktif slotta `<yol>.gz` varsa → gzip, chunked (`:651-654`).
   b. aktif slotta `<yol>` düz varsa → düz, chunked (`:655-658`).
   Her iki kontrol `ext_flash_active_get_file_size()` ile ayrı LittleFS aramasıdır; sonra `send_active_slot_file_chunked` tekrar `fopen` eder (`:558-590`): 4096 baytlık `malloc` tampon, `httpd_resp_send_chunk` döngüsü, `sys_log_mark_event(SL_EV_GUI_SERVE, boyut)`.
2. Gömülü tablo `s_embedded_files[12]` (`:89-102`): index/style/app ve 6 dil dosyası **gzip** (derlemede `gzip_gui.py`, `ext_flash/CMakeLists.txt:12-52`), `config.json` düz (`EMBED_TXTFILES`), iki PNG düz. Gömülü gzip kopya istemcinin `Accept-Encoding`'ine **bakılmaksızın** `Content-Encoding: gzip` ile gönderilir (`:669`; yorum "tum tarayicilar gzip kabul eder").
3. Hiçbiri yoksa 404 "Dosya bulunamadi" (text/html).

Not: "aktif slot" `/slot_a` veya `/slot_b` bağlama noktasıdır (`ext_flash.c:45,50`); `/ext` ise kullanıcı verisi bağlama noktasıdır (`ext_flash.c:54`). URL öneki `/ext/` ile dosya sistemi `/ext/` **aynı şey değildir**; `/ext/mail/...` dosyaları beyaz liste sayesinde sunulmaz.

### 3.4 Başlıklar (`set_static_headers`, `:593-614`)
| Uzantı | Content-Type | Cache-Control |
|---|---|---|
| `.html` | text/html | `no-cache` |
| `.css` | text/css | `public, max-age=31536000, immutable` |
| `.js` | application/javascript | immutable (1 yıl) |
| `.json` | application/json | `no-cache` |
| `.png` | image/png | immutable |
| `.ico` | image/x-icon | immutable |
| diğer/yok | text/plain | `no-cache` |

gzip ise `Content-Encoding: gzip` + `Vary: Accept-Encoding`. Immutable önbellek, GUI'nin `?v=<sürüm>` sorgusuyla kırılmasına bağımlıdır. ETag/Last-Modified yok. Gömülü yolda `httpd_resp_set_type` iki kez çağrılır (`:669-670`, zararsız).

---

## 4. API uçları — eksiksiz tablo

Rota tablosu `s_routes[55]` (`web_server.c:2541-2597`). Tüm GET/POST'lar; başka yöntem IDF tarafından 405 (text/html) ile reddedilir. "Gövde ≤N" = `read_body` tampon boyutu − 1 (daha uzun gövde **sessizce kesilir**, §5). "400 metin" = `httpd_resp_send_err` (text/html gövde, JSON değil). Aksi belirtilmedikçe `{status:error}` yanıtları **HTTP 200** ile döner.

### 4.1 Kimlik doğrulama

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/auth/status` | GET | hayır | — | `{"enabled":b,"logged_in":b,"ttl_min":u,"protect":b,"lock_min":u}` | — | `auth_request_ok` | Geçerli oturumun `last_seen` süresini **uzatır** | 314-323 |
| `/api/auth/login` | POST | hayır | gövde ≤255: `{"password":s≤95}` | `{"status":"ok","token":"<32hex>"}` + `Set-Cookie: ls_sess=...`; auth kapalıysa `{"status":"ok","enabled":false}` | 429 `{"status":"locked","retry_s":n}`; 401 `{"status":"error","message":"Parola yanlis"}`; 400 metin "Veri alinamadi" | `auth_hash_password` (mbedtls_sha256, esp_efuse_mac), `ct_equal`, `auth_session_create`, `auth_set_cookie`, `sys_log_write` | `s_auth_fail`/kilit; sys_log satırı | 326-366 |
| `/api/auth/logout` | POST | evet | çerez | `{"status":"ok"}` + `Set-Cookie: ls_sess=; Max-Age=0` | — | `auth_cookie_token` | Oturum RAM'den silinir | 369-380 |
| `/api/auth/set` | POST | handler | gövde ≤383: `{"current":s≤95,"new":s≤95,"ttl_min":1-1440,"protect":b,"lock_min":1-60}` (hepsi isteğe bağlı; `new` alanı varsa parola değişir, `""` → kaldırılır) | `{"status":"ok","enabled":b}` (+ yeni parola ayarlandıysa yeni Set-Cookie) | 401 `{"status":"error","message":"Oturum gerekli"}` (parola varken oturumsuz); 200 `message`: "Mevcut parola yanlis" / "Parola en az 4 karakter" / "Kaydedilemedi"; 400 metin | `ls_json_get_int/bool`, `auth_store_session_cfg`, `auth_hash_password`, `auth_store_password`, `auth_session_create`, `auth_set_cookie`, `sys_log_write` | NVS `ls_auth` (`hash`, `ttl_min`, `protect`, `lock_min`); tüm oturumlar düşer; ttl/protect/lock **parola kontrolünden önce** kaydedilir (`:404-407`) | 384-435 |

### 4.2 Sistem / cihaz

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/system/info` | GET | evet | — | `{"device_name","ip","ap_ssid","ap_ip":"192.168.4.1"(sabit),"sta_ssid","connected":b,"mdns":"<ad>.local","ext_flash":b,"slot_a_total":KB,"slot_a_free":KB,"user_data_total":KB,"user_data_free":KB,"auth_enabled":b}` — SSID'ler **escape edilmez** | — | `ext_flash_is_slot_a_mounted/get_slot_a_info/is_user_data_mounted/get_info/is_mounted`, `wifi_manager_get_device_name/get_ip/get_ap_ssid/get_sta_ssid/is_connected` | — | 686-729 |
| `/api/system/detail` | GET | evet | — | `{"chip","cores","cpu_mhz","flash_kb","flash_used_kb","heap_total","heap_free","heap_min","uptime","rssi","reset_reason","fw_version","mdns","slot_a_total","slot_a_free","slot_b_kb":1024(sabit),"user_total","user_free","boot_count","short_boots","last_cause","last_cat","prev_uptime","tx_now","tx_max","ap_clients","ext_flash_mb"}` (resp[1024]) | — | `system_info_get`, `ext_flash_*`, `sys_log_boot_count/short_boots/last_cause/last_cause_cat/prev_uptime`, `wifi_manager_get_tx_power_now/max/get_ap_client_count`, `ext_flash_get_chip_size` | — | 1128-1201 |
| `/api/device/restart` | POST | evet | — | `{"status":"ok"}` | — | `sys_log_set_restart_reason(SL_RST_USER_WEB)`, `sys_log_write` | 500 ms sonra `esp_restart()` (httpd görevinden) | 1204-1213 |
| `/api/device/factory-reset` | POST | evet | — | `{"status":"ok"}` | — | `sys_log_set_restart_reason(SL_RST_FACTORY)`, `sys_log_write`, `nvs_flash_erase`, `ext_flash_format_user_data` | **Tüm NVS** silinir (WiFi, SMTP, Telegram, timer, röle, auth, API anahtarı…), `/ext` (loglar + mail ekleri) biçimlenir, slot A/B'ye dokunulmaz, restart. Dönüş değerleri denetlenmez | 1216-1232 |
| `/api/logs` | GET | evet | — | chunked: `{"boot_count","short_boots","boot_epoch","last_cause","last_cat","prev_uptime","logs":[…≤150, en yeni önce]}` + `Cache-Control: no-cache` | 500 metin "Bellek yetersiz" | `sys_log_read_tail_json(150)`, `sys_log_*` | — | 1289-1311 |
| `/api/logs/clear` | POST | evet | — | `{"status":"ok"}` | 200 `{"status":"error","message":"Harici flash kullanilamiyor"}` | `sys_log_clear` | `/ext/logs/events.log` silinir | 1314-1322 |

### 4.3 WiFi

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/wifi/config` | GET | evet | — | `{"connected":b,"ip","sta_ssid"(escape),"static":{"ip","gateway","subnet","dns"},"backup_ssid"(escape),"backup_static":{…},"has_backup":b,"ap_enabled":b,"ap_clients":n,"rssi":n,"tx_max":n,"tx_now":n,"tx_adaptive":b}` — parolalar dönmez | — | `wifi_manager_get_primary_static/get_backup_static/get_sta_ssid/get_backup_ssid/is_connected/get_ip/has_backup_config/is_ap_enabled/get_ap_client_count/get_sta_rssi/get_tx_power_max/now/get_tx_adaptive` | — | 816-855 |
| `/api/wifi/connect` | POST | evet | gövde ≤383: `{"ssid":s≤32,"password":s≤64,"static_ip":s≤15,"gateway","subnet","dns"}`; `ssid` boş → birincil ayar **silinir** | `{"status":"ok","message":"Baglaniliyor: <ssid>"}` (ssid escape edilmez) / `"WiFi ayarlari silindi"` | 200 `{"status":"error","message":"Hata: ESP_ERR_…"}`; 400 metin | `wifi_manager_clear_primary`, `wifi_manager_connect_sta` (asenkron: NVS `wifi_cfg/p_*` yazar, bağlantıyı başlatır; `wifi_manager.c` — parola boşsa aynı SSID'nin kayıtlı parolası korunur) | NVS; STA bağlantı denemesi; ESP_LOGI SSID + parola uzunluğu | 732-771 |
| `/api/wifi/backup/save` | POST | evet | aynı alanlar; `ssid` boş → yedek silinir | `"Yedek WiFi kaydedildi: <ssid>"` / `"Yedek WiFi ayarlari silindi"` | aynı | `wifi_manager_clear_backup`, `wifi_manager_save_backup` | NVS `wifi_cfg/b_*` | 774-813 |
| `/api/wifi/ap/set` | POST | evet | gövde ≤63: `{"enabled":b}` (yoksa false) | `{"status":"ok","ap_enabled":b}` | 200 `"WiFi bagli degil, AP kapatilamaz"` (ESP_ERR_INVALID_STATE) / `esp_err_to_name`; 400 metin | `wifi_manager_set_ap_enabled`, `is_ap_enabled` | WiFi modu APSTA↔STA; NVS `wifi_cfg/ap_enabled` | 858-885 |
| `/api/wifi/txpower` | POST | evet | gövde ≤127: `{"max_dbm":8-20,"adaptive":b}` (eksikse mevcut değer) | `{"status":"ok","tx_max","tx_now","tx_adaptive"}` | 200 `"Gecersiz deger (8-20 dBm)"`; 400 metin | `wifi_manager_set_tx_power` | NVS + anında uygulanır | 888-912 |

### 4.4 Zamanlayıcı

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/timer/get` | GET | evet | — | `{"enabled":b,"remaining_seconds":n,"total_seconds":n,"vacation_enabled":b,"vacation_remaining":n,"vacation_total":n,"alarm_count":n}` | — | `timer_manager_get_state` | — | 915-933 |
| `/api/timer/set` | POST | evet | gövde ≤127, `action` alanına göre çok amaçlı: `"cancel"`; `"restart"`; `"vacation_set"`+`"days"`(1-60); `"vacation_cancel"`; `"set_alarms"`+`"count"`(0-10); **action yok/başka** → `{"unit":"gun"|"saat"|"dakika","value":1-60}` (sayı veya sayı-string) | cancel `{"status":"ok","remaining_seconds":0}`; restart/set `{"status":"ok","remaining_seconds","total_seconds"}`; vacation_set `{"status":"ok","vacation_remaining","vacation_total"}`; vacation_cancel/set_alarms `{"status":"ok"}` | 200 `message`: "Onceden ayarlanmis sure yok" / "Tatil ayarlanamadi" / "Gecersiz alarm sayisi" / "Timer ayarlanamadi"; **400 metin** "Deger 1-60 arasi olmali"; 400 metin | `timer_manager_cancel/restart/set_vacation/cancel_vacation/set_alarm_count/set/get_state`, `sys_log_write`, `web_server_send_timer_event` | timer_manager NVS'si; SSE yayını | 936-1038 |

### 4.5 SMTP / e-posta içerikleri

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/smtp/get` | GET | evet | — | `{"configured":b,"server","port":n,"user"}` — `api_key` dönmez | — | `smtp_client_get_config/has_config` | — | 1041-1057 |
| `/api/smtp/save` | POST | evet | gövde ≤383: `{"server":s≤63,"port":n(0→465, aralık denetimi yok),"user":s≤63,"api_key":s≤63 (boş → mevcut korunur)}`; CR/LF temizlenir | `{"status":"ok","message":"SMTP ayarlari kaydedildi"}` | 200 `"Server ve email gerekli"` / `"Kaydetme hatasi"`; 400 metin | `ls_strip_crlf`, `smtp_client_get_config/save_config` | smtp_client NVS'si | 1060-1105 |
| `/api/smtp/test` | POST | evet | — | `{"status":"ok","message":"Test maili gonderildi"}` | 200 `"SMTP yapilandirilmamis"` / ESP_ERR_INVALID_ARG→`"Auth hatasi - email/sifre yanlis"` / `"Baglanti hatasi - sunucu/port kontrol edin (ESP_…)"` | `smtp_client_has_config`, **`smtp_client_send_test` (bloklayan, httpd görevinde TLS)** | Gerçek e-posta gider; sunucu süre boyunca kilitli | 1108-1125 |
| `/api/early-mail/get` | GET | evet | — | `{"configured":b,"email","subject","body"}` (escape) | — | NVS doğrudan (`ls_ew_mail`) | — | 1463-1492 |
| `/api/early-mail/save` | POST | evet | gövde ≤511: `{"email":s≤63,"subject":s≤63,"body":s≤255}`; e-posta biçim denetimi yok; CR/LF temizliği email+subject | `{"status":"ok"}` | 200 `"NVS hatasi"`; 400 metin | `ls_strip_crlf`, NVS doğrudan | NVS `ls_ew_mail/{email,subject,body}` — **eksik alanlar boş yazılır** | 1495-1525 |
| `/api/trigger-mail/get` | GET | evet | — | `{"count":n,"groups":[{"id":i,"name","subject","body","recipients"}…≤10]}` (14 KB heap) | 500 metin | NVS doğrudan (`ls_tg_mail`) | — | 1532-1588 |
| `/api/trigger-mail/save` | POST | evet | gövde ≤1023: `{"id":n (yok/"" → yeni; 0..count-1 → güncelle),"name":s≤63,"subject":s≤63,"body":s≤255,"recipients":s≤255}`; CR/LF temizliği subject+recipients | `{"status":"ok","id":n}` | 200 `"Maksimum 10 grup"` / `"Gecersiz grup ID"` / `"NVS hatasi"`; 400 metin | NVS doğrudan | NVS `ls_tg_mail/count, g{i}_name/subj/body/rcpt` | 1591-1655 |
| `/api/trigger-mail/delete` | POST | evet | gövde ≤63: `{"id":n}` — **id yoksa 0 varsayılır** (`atoi("")`) | `{"status":"ok"}` | 200 `"Gecersiz grup ID"` / `"NVS hatasi"`; 400 metin | NVS doğrudan; `opendir/readdir/remove/rmdir/rename` | NVS kayıtları kaydırılır; `/ext/mail/g{id}` silinir, `g{i+1}`→`g{i}` yeniden adlandırılır | 1658-1762 |
| `/api/trigger-mail/upload` | POST | evet | başlıklar `X-Group-Id` (0-9), `X-Filename` (≤63; `/`,`\`,`..` yasak); **ham ikili gövde** 1 B..2 MB; Content-Type denetimi yok | `{"status":"ok"}` | 400 metin: "X-Group-Id ve X-Filename header gerekli" / "Gecersiz grup veya dosya adi" / "Gecersiz dosya adi" / "Dosya cok buyuk (max 2MB)"; 500 metin; 200 `"Harici flash kullanilamiyor"` / `"Dosya olusturulamadi"` / `"Dosya yazma hatasi"` | `ext_flash_is_user_data_mounted`, `mkdir`, `fopen/fwrite`, `httpd_req_recv` (1 KB parçalar, 5 zaman aşımı) | `/ext/mail/g{N}/<ad>` yazılır; aynı ad üzerine yazar; dosya sayısı/toplam kota sınırı yok | 2065-2152 |
| `/api/trigger-mail/files` | GET | evet | `?group=N` (atoi, aralık denetimi yok) | `{"files":[{"name":s,"size":n}…]}` (2 KB heap, ad escape edilmez) | 500 metin | `opendir/readdir/stat` | — | 2155-2202 |
| `/api/trigger-mail/file-delete` | POST | evet | gövde ≤127: `{"group":n,"name":s≤63}` (traversal denetimi) | `{"status":"ok"}` | 200 `"Gecersiz dosya adi"` / `"Dosya silinemedi"`; 400 metin | `remove` | `/ext/mail/g{N}/<ad>` silinir | 2205-2235 |

### 4.6 Telegram

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/telegram/get` | GET | evet | — | `{"configured":b,"chat_id","message"}` — token dönmez | — | `telegram_client_get_config/has_config` | — | 1839-1856 |
| `/api/telegram/save` | POST | evet | gövde ≤511: `{"token":s≤71 (boş → mevcut korunur),"chat_id":s≤31 (zorunlu),"message":s≤255}` | `{"status":"ok","message":"Telegram ayarlari kaydedildi"}` | 200 `"Chat ID gerekli"` / `"Kaydetme hatasi"`; 400 metin | `telegram_client_get_config/save_config` | NVS `ls_telegram` | 1859-1896 |
| `/api/telegram/test` | POST | evet | — | `{"status":"ok","message":"Test mesaji gonderildi"}` | 200 `"Telegram yapilandirilmamis"` / INVALID_ARG→`"Auth hatasi - token yanlis"` / `"Gonderim hatasi (ESP_…)"` | `telegram_client_has_config`, **`telegram_client_send` (bloklayan HTTPS, 10 s zaman aşımı, `telegram_client.c:114,126`)** | Gerçek mesaj gider | 1899-1916 |

### 4.7 Webhook / MQTT

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/api-config` | GET | evet | — | `{"webhook":{"url","method","headers","body","configured":b},"mqtt":{"url","topic","user","has_pass":b,"payload","configured":b}}` (4 KB heap; `mq_pass` dönmez) | 500 metin | `api_client_get_config/has_webhook/has_mqtt` | — | 442-469 |
| `/api/api-config` | POST | evet | gövde ≤2047 (heap): `{"wh_url":s≤255 (http(s):// zorunlu),"wh_method":"GET"\|"PUT"\|diğer→"POST","wh_headers":s≤255,"wh_body":s≤511,"mq_url":s≤127 (mqtt(s)://),"mq_topic":s≤127,"mq_user":s≤63,"mq_pass":s≤63 (boş → korunur),"mq_payload":s≤255}` | `{"status":"ok"}` | 200 `"Webhook URL http:// veya https:// ile baslamali"` / `"MQTT URL mqtt:// veya mqtts:// ile baslamali"` / `"Kaydedilemedi"`; 400/500 metin | `api_client_get_config/save_config` (9 ayrı cJSON parse) | NVS `ls_api` | 472-509 |
| `/api/api-config/test` | POST | evet | gövde ≤63: `{"target":"mqtt"}` (başka her şey → webhook); dönüş değeri denetlenmez | `{"status":"ok","message":"Test gonderildi"}` | 200 `"MQTT yapilandirilmamis"` / `"Webhook yapilandirilmamis"` / `"Gonderim hatasi (ESP_…)"` | `api_client_has_mqtt/has_webhook`, **`api_client_send_mqtt("test")` / `send_webhook("test")` (bloklayan; `api_client.h:35,38`)** | Dış sisteme istek gider | 512-530 |

### 4.8 Röle / aksiyonlar

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/relay/get` | GET | evet | — | `{"duration":n,"pulse":n,"delay":n,"inverted":b}` | — | `output_get_relay_config/get_inverted` | — | 1235-1246 |
| `/api/relay/set` | POST | evet | gövde ≤255: `{"duration":0-3600,"pulse":0-60,"delay":0-60,"inverted":b?}` — **eksik sayısal alan 0 kabul edilir** | `{"status":"ok"}` | **400 metin** "Gecersiz deger"; 400 metin | `output_set_inverted` (alan varsa), `output_set_relay_config`, `output_save_config` | output NVS'si | 1249-1278 |
| `/api/relay/test` | POST | evet | — | `{"status":"ok"}` | — | `output_relay_trigger` — yapılandırmaya göre **gerçek tetikleme** (delay→pulse/sustained; `output.h:49-51`), yorumdaki "kisa pulse" yanıltıcı | Röle çalışır | 1281-1286 |
| `/api/actions/get` | GET | evet | — | `{"early_telegram":b,"early_mail":b,"early_api":b,"trigger_mail":b,"trigger_api":b,"trigger_relay":b}` (varsayılan: hepsi false, `trigger_relay` true) | — | NVS doğrudan (`ls_actions`) | — | 1769-1797 |
| `/api/actions/save` | POST | evet | gövde ≤255: aynı 6 anahtar, **yalnızca gelenler yazılır** (kısmi güncelleme) | `{"status":"ok"}` | 200 `"NVS hatasi"`; 400 metin | NVS doğrudan | NVS `ls_actions/{early_tg,early_mail,early_api,trig_mail,trig_api,trig_relay}` — `main.c` bunları `nvs_get_flag` ile doğrudan okur | 1800-1832 |

### 4.9 Uzaktan sıfırlama

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/reset/config` | GET | evet | — | `{"enabled":b,"has_key":b,"masked_key":"ls_a3f7****"}` | — | NVS doğrudan (`ls_reset`) | — | 1923-1957 |
| `/api/reset/config` | POST | evet | gövde ≤63: `{"enabled":b}` | `{"status":"ok","enabled":b}` (NVS hatası sessiz) | 400 metin | NVS doğrudan | NVS `ls_reset/enabled` | 1960-1984 |
| `/api/reset/generate` | POST | evet | — | `{"status":"ok","api_key":"ls_xxxxxxxx"}` (tek seferlik tam gösterim) | 200 `"NVS hatasi"` | `esp_random`, NVS doğrudan | NVS `ls_reset/api_key` + `enabled=1` | 1987-2009 |
| `/api/reset` | **GET** | **hayır** | `?key=<≤15>` | `{"status":"ok","message":"Timer sifirlandi"}` | **403** `"Uzaktan sifirlama devre disi"`; **401** `"Gecersiz API anahtari"`; 200 `"Timer sifirlanamadi"` | NVS doğrudan, `timer_manager_restart`, `sys_log_write("api")`, `web_server_send_timer_event` | Geri sayım yeniden başlar; SSE; hız sınırı yok | 2012-2058 |

### 4.10 GUI OTA

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/gui-ota/info` | GET | evet | — | `{"active_slot":"a"\|"b","active_version","inactive_version","inactive_has_data":b,"slot_b_mounted":b,"busy":b}` | — | `ext_flash_get_active_mount/get_inactive_mount/get_active_slot/is_slot_b_mounted`, `gui_ota_is_busy`; `<mount>/config.json` doğrudan `fopen` (256 B tampon; mevcut dosya 67 B) | — | 2242-2296 |
| `/api/gui-ota/start` | POST | evet | gövde ≤255 isteğe bağlı: `{"source":"custom","repo_url":s≤127,"branch":s≤31}` | `{"status":"ok","message":"OTA baslatildi"}` | 200 `"OTA zaten calisiyor"` / `"OTA baslatilamadi"` | `gui_ota_is_busy`, `gui_ota_start` (arka plan görevi 10 KB, `gui_ota.c:477`) | İnaktif slot silinip GitHub'dan yazılır | 2299-2330 |
| `/api/gui-ota/status` | GET | evet | — | `{"state":0-7,"file_index","file_count","progress_pct","message"(escape),"remote_version","current_version"}` (`gui_ota_state_t`: 0 IDLE,1 CHECKING,2 DOWNLOADING,3 VERIFYING,4 SWITCHING,5 DONE,6 ERROR,7 NO_UPDATE) | — | `gui_ota_get_status` | — | 2333-2360 |
| `/api/gui-ota/rollback` | POST | evet | gövde yok sayılır | `{"status":"ok","active_slot":"a"\|"b"}` | 200 `"Diger slot'ta veri yok"` (NOT_FOUND) / `"Rollback basarisiz"` (meşgulken INVALID_STATE dahil) | `gui_ota_rollback` → `ext_flash_set_active_slot` (`gui_ota.c:494-519`) | Aktif slot değişir; **restart yok**, bir sonraki statik istek yeni slottan gelir | 2363-2384 |

### 4.11 Firmware OTA

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/fw-ota/info` | GET | evet | — | `{"running_partition":"ota_0"\|"ota_1","current_version","can_rollback":b,"rollback_version","busy":b}` | — | `fw_ota_get_running_label/get_rollback_version/can_rollback/is_busy/get_status` | — | 2391-2415 |
| `/api/fw-ota/check` | POST | evet | gövde ≤511 isteğe bağlı: `{"source":"custom","url":s≤299}` | `{"status":"ok","message":"Kontrol basladi"}` | 200 `"OTA zaten calisiyor"` / `"Baslatilamadi"` | `fw_ota_is_busy`, `fw_ota_check(url\|NULL)` (arka plan görevi, `fw_ota.c:393`) | — | 2418-2453 |
| `/api/fw-ota/start` | POST | evet | — | `{"status":"ok","message":"Indirme basladi"}` | 200 `"Once versiyon kontrolu yapiniz"` (INVALID_STATE) / `"Baslatilamadi"` | `fw_ota_start` (UPDATE_AVAILABLE şart; görev `fw_ota.c:423`) | Diğer OTA bölümüne yazılır; DONE sonrası kullanıcı restart eder | 2456-2470 |
| `/api/fw-ota/status` | GET | evet | — | `{"state":0-6,"progress_pct","message"(escape),"remote_version","current_version"}` (`fw_ota_state_t`: 0 IDLE,1 CHECKING,2 UPDATE_AVAILABLE,3 NO_UPDATE,4 DOWNLOADING,5 DONE,6 ERROR) | — | `fw_ota_get_status` (mutex'li) | — | 2473-2496 |
| `/api/fw-ota/rollback` | POST | evet | — | `{"status":"ok","message":"Rollback ayarlandi, restart gerekli"}` | 200 `"Rollback partition bulunamadi"` / `"Rollback hatasi"` | `fw_ota_rollback` | Önyükleme bölümü değişir; restart kullanıcıya bırakılır | 2499-2515 |

### 4.12 Gerçek zamanlı ve statik

| Yol | Yöntem | Auth | İstek | Yanıt | Hatalar | Modül çağrıları | Yan etkiler | Satır |
|---|---|---|---|---|---|---|---|---|
| `/api/events` | GET | evet | EventSource (çerez same-origin ile gider) | Ham yazılmış `HTTP/1.1 200` + `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `Connection: keep-alive`, `Access-Control-Allow-Origin: *`; ilk veri `retry: 3000\n` `data: connected\n\n`; sonra her olayda `data: {timer/get ile aynı JSON}\n\n` (`sse_do_send`, `:1389-1420`) | 401 JSON (dispatcher); soket yazılamazsa ESP_FAIL | `httpd_req_to_sockfd`, `httpd_socket_send`, `httpd_sess_trigger_close`, `timer_manager_get_state`, `httpd_sess_update_lru_counter` | `s_sse_fds[3]`; 4. istemci gelince **indeks 0** kapatılır (`:1342-1347`); Content-Length/chunked yok (kapanana kadar akış) | 1329-1371 |
| `/` | GET | hayır | — | `/index.html` (§3) | 404 metin | `serve_static_path` | `sys_log_mark_event(SL_EV_GUI_SERVE)` | 680-683 |
| `/ext/*` | GET | hayır | yol (≤127, `?` sonrası atılır) | §3 | 404 metin | `serve_static_path` | aynı | 1443-1456 |

Çapraz kontrol: `ALTE/GUI/app.js` içinde `/api/...` dizgeleri sayıldı; 55 rotanın **hepsi** GUI tarafından çağrılıyor (dosya başı kullanım: `timer/set` 6, `relay/set` 3, `device/restart` 3, kalanı 1-2). Ölü rota yok.

---

## 5. JSON işleme

- **Kütüphane**: istek ayrıştırma cJSON üzerinden `ls_util` (`ls_json_get_str/int/bool`, `ALTE/HW/components/ls_util/ls_util.c:7-75`); yanıt üretimi **tamamen elle** `snprintf` + `ls_json_escape` (`web_server.c:107-108` makroları). cJSON yalnızca okumada kullanılıyor, yazmada değil.
- **Her alan okuması gövdeyi baştan parse eder**: `ls_json_get_*` her çağrıda `cJSON_Parse` + `cJSON_Delete` yapar. Sonuç: `/api/api-config` POST'ta 2 KB gövde **9 kez** parse edilir (`:487-495`), `/api/wifi/connect` 6 kez, `/api/actions/save` 6 kez, `/api/auth/set` 5 kez, `/api/trigger-mail/save` 5 kez. Heap churn + CPU; tek görevli sunucuda gecikme.
- **`ls_json_get_str` tip esnekliği**: sayı → ondalıksız metin, bool → "true"/"false" (`ls_util.c:19-33`). Bu yüzden `"port":465` ve `"port":"465"` ikisi de çalışır; `"days"`, `"count"`, `"value"`, `"id"`, `"group"` string tamponlara alınıp `atoi` ile çevrilir (`:973-974, 1000-1001, 1015-1017, 1602-1605, 1668-1669, 2219`).
- **Gövde okuma** `read_body` (`:533-548`): `content_len == 0` → 0 (tampon boş); `content_len >= size` → **yalnızca `size-1` bayt okunur, gerisi okunmaz**, dönüş pozitif; 5 zaman aşımı denemesi (5 × `recv_wait_timeout` 5 s = 25 s'ye kadar bloklama). Kesilmiş gövde geçersiz JSON olur → `cJSON_Parse` NULL → **tüm alanlar "yok" sayılır**, tamponlar sıfır-başlangıç değerinde kalır. Somut sonuçlar §8.2.
- **Content-Type** denetimi yok; `Content-Length` olmayan/chunked istek gövdesi desteklenmez (IDF httpd sınırı).
- **Tampon sınırları (tüm POST uçları)**: 32 (gui-ota/rollback), 64 (api-config/test, ap/set, trigger-mail/delete, reset/config), 128 (txpower, timer/set, file-delete), 256 (login, relay/set, actions/save, gui-ota/start), 384 (auth/set, wifi/connect, backup/save, smtp/save), 512 (early-mail/save, telegram/save, fw-ota/check), 1024 (trigger-mail/save), 2048 heap (api-config), 2 MB akış (upload).
- **`send_json`** (`:111-115`): `Content-Type: application/json` + `httpd_resp_send(strlen)`. `Cache-Control` eklemez (yalnızca `/api/logs` `no-cache`, dispatcher 401 `no-store`). `/api/logs/clear` `send_json` yerine aynı şeyi elle yapar (`:1317-1321`).
- **Yanıt biçimleri (tutarsız)**:
  1. Başarı/hata gövdesi `{"status":"ok"|"error","message":"…"}` ve **HTTP 200** (çoğunluk).
  2. Dispatcher 401: `{"status":"error","error":"auth_required"}` — `message` yerine `error` anahtarı.
  3. Login 401: `{"status":"error","message":"Parola yanlis"}`; 429: `{"status":"locked","retry_s":n}` — `status` değeri üçüncü bir sözcük.
  4. `httpd_resp_send_err` → **HTTP 400/500 + `text/html`** gövde (`httpd_txrx.c` `HTTPD_TYPE_TEXT`), GUI'nin JSON beklediği yerde: tüm "Veri alinamadi" (17 handler), relay/set "Gecersiz deger" (`:1263`), timer/set "Deger 1-60 arasi olmali" (`:1019`), upload başlık/boyut hataları (`:2072-2091`), malloc hataları.
  5. GET'ler `status` alanı olmadan ham nesne döner; `/api/logs` chunked.
  6. Mesajlar sabit **Türkçe, diyakritiksiz** ("Baglaniliyor", "Gecersiz"); GUI 6 dilli → sunucu mesajları çevrilemez.
- **Escape eksikleri** (çıktıda JSON bozulabilir): `/api/system/info` `device_name/ap_ssid/sta_ssid` (`:715-718`), `/api/wifi/connect` ve `backup/save` yanıtındaki `ssid` (`:764, 806`), `/api/trigger-mail/files` `d_name` (`:2191`), OTA `remote_version/current_version` (`:2356-2357, 2492-2493`).

---

## 6. Uzun süren işler

| İş | Yürütme modeli | İlerleme sorgusu | Satır |
|---|---|---|---|
| Firmware OTA kontrol/indir | `fw_ota_check`/`fw_ota_start` ayrı FreeRTOS görevi (`fw_ota.c:393,423`), handler hemen döner | GUI `GET /api/fw-ota/status` ile polling (`state`, `progress_pct`, `message`) | 2418-2470 |
| GUI OTA | `gui_ota_start` görev (`gui_ota.c:477`, 10 KB yığın) | `GET /api/gui-ota/status` polling | 2299-2360 |
| GUI/FW rollback | Senkron ama kısa (slot işaretleme / bölüm seçimi) | — | 2363, 2499 |
| **SMTP test** | **Senkron, httpd görevinde**: `smtp_client_send_test` → `smtp_client_send` → `mbedtls_ssl_handshake` çağıranın yığınında (`smtp_client.c:389,411,626,632`). Başlık açıkça "bloklayan cagri, ayri task'ta calistirin" der (`smtp_client.h:26`) ama ayrı görev yok | Yok; fetch tamamlanana kadar bekler, bu sırada **tüm HTTP sunucusu durur** (SSE push ayrı görevden gider) | 1114 |
| **Telegram test** | Senkron HTTPS, 10 s zaman aşımı (`telegram_client.c:114,126`); başlık "bloklayan, ayri task'ta" (`telegram_client.h:25`) | Yok | 1905 |
| **Webhook/MQTT test** | Senkron; `api_client.h:35,38` "bloklayan; ayri gorevde cagirin"; MQTT bağlan-yayınla-kapat | Yok | 521, 524 |
| WiFi bağlan | Asenkron (`wifi_manager_connect_sta` NVS yazar, `do_connect` başlatır, döner) | GUI `GET /api/wifi/config` → `connected`/`ip` | 760 |
| WiFi tarama | **Yok** — sunucuda tarama ucu bulunmuyor; GUI SSID'yi elle alıyor (`wifi_manager_auto_connect` içindeki tarama API'ye açılmamış) | — | — |
| Restart / factory reset | Handler içinde 500 ms bekleyip `esp_restart` | — | 1210, 1229 |
| Dosya yükleme (≤2 MB) | Handler içinde akış halinde LittleFS yazımı; süresi boyunca sunucu meşgul | Yok | 2126-2140 |
| Olay akışı | SSE; `web_server_send_timer_event` → `sse_send` görevi (`ulTaskNotifyTake`) → `httpd_socket_send` | — | 1423-1440 |

---

## 7. Yapılandırma okuma/yazma modeli

- **Parça parça**: tek bir "ayarlar" JSON'u yok. Her alan için GET/POST çifti:
  `auth/status`↔`auth/set`, `wifi/config`↔`wifi/connect`+`wifi/backup/save`+`wifi/ap/set`+`wifi/txpower`, `timer/get`↔`timer/set`, `smtp/get`↔`smtp/save`, `telegram/get`↔`telegram/save`, `api-config` GET↔POST, `relay/get`↔`relay/set`, `early-mail/get`↔`save`, `trigger-mail/get`↔`save`/`delete`, `actions/get`↔`save`, `reset/config` GET↔POST, `gui-ota/info`, `fw-ota/info`, `system/info`+`system/detail` (salt okunur).
- **Güncelleme semantiği tutarsız**: `actions/save` kısmi (yalnızca gelen anahtarlar); `relay/set` tam yerine koyma (eksik → 0); `smtp/save`, `telegram/save`, `api-config` tam yerine koyma ama **gizli alan boşsa eski korunur** (`:1085-1090, 1876-1881, 497`); `early-mail/save` tam yerine koyma (eksik → boş yazılır); `auth/set` kısmi; `wifi/connect` boş SSID = sil.
- **Gizliler asla okunmaz**: `api_key` (SMTP), `token` (Telegram), `mq_pass`, WiFi parolaları, parola hash'i. `has_pass`/`configured` bayrakları döner.
- **Sunucunun doğrudan sahiplendiği NVS ad alanları** (iş mantığı HTTP katmanında):
  | Ad alanı | Anahtarlar | Handler |
  |---|---|---|
  | `ls_auth` | `hash`(str64), `ttl_min`(u16), `protect`(u8), `lock_min`(u8) | auth_* (`:130-229`) |
  | `ls_ew_mail` | `email`, `subject`, `body` (str) | `:1469-1521` |
  | `ls_tg_mail` | `count`(u8), `g{0-9}_name/_subj/_body/_rcpt` (str) | `:1548-1735` |
  | `ls_actions` | `early_tg`, `early_mail`, `early_api`, `trig_mail`, `trig_api`, `trig_relay` (u8) | `:1775-1828`; `main.c` de okur |
  | `ls_reset` | `api_key`(str), `enabled`(u8) | `:1931-2023` |
- **Modüller üzerinden yazılanlar**: `wifi_cfg` (wifi_manager: `p_*`, `b_*`, `ap_enabled`, TX), `ls_api` (api_client), SMTP ve `ls_telegram` (ilgili istemciler), röle (output_save_config), timer (timer_manager). Ayrıntılı NVS envanteri başka belgede.
- **Dosya sistemi**: `/ext/mail/g{N}/<dosya>` ekleri (upload/files/file-delete/delete), `/ext/logs/events.log` (sys_log), `/slot_a|/slot_b/config.json` sürüm okuma.

---

## 8. Gördüğüm sorunlar

### 8.1 Güvenlik
1. **Düz metin HTTP + parolasız AP** — Port 80, TLS yok (`:2610`); AP şifresiz (`wifi_manager.h:26`). `/api/auth/login` parolası ve `ls_sess` çerezi radyo menzilindeki herkes tarafından dinlenebilir. Tarayıcı parolası LAN'da yalnızca "kazara erişim" önler, saldırgana karşı değil.
2. **Parola ayarlı değilken CSRF/DNS-rebinding'e tamamen açık** — Origin/Host denetimi yok, Content-Type zorunlu değil (`read_body` ne gelirse okur), POST'lar "basit istek" olarak preflight'sız gider. Herhangi bir web sayfası `<form action="http://192.168.4.1/api/device/factory-reset" method=POST>` ile cihazı sıfırlatabilir; `relay/test`, `timer/set` `cancel` aynı şekilde. Parola varken `SameSite=Strict` (`:308`) modern tarayıcılarda korur.
3. **`auth_set_cookie` use-after-return** (`:305-311`) — `cookie[160]` yerel tampon `httpd_resp_set_hdr`'a verilir; IDF değeri **kopyalamaz**, işaretçiyi saklar (`httpd_txrx.c` `ra->resp_hdrs[..].value = value`) ve başlıkları `httpd_resp_send` anında yazar. Fonksiyon dönünce işaretçi ölü yığın çerçevesini gösterir; login'de araya `sys_log_write` (`:362`) ve `snprintf` (`:364`) girer. `Set-Cookie` değeri bozuk/rastgele gidebilir. `auth/set` yolunda da aynı (`:427`→`:432-434`). Davranış derleyici/çerçeve düzenine bağlı; cihazda doğrulanmalı (§10).
4. **Küresel giriş kilidi** — `s_auth_fail`/`s_auth_lock_until` IP'den bağımsız (`:145-146`). 5 yanlış deneme meşru kullanıcıyı `lock_min` dakika kilitler (DoS). Kilit RAM'de: restart sıfırlar.
5. **Tek tur SHA-256(MAC:parola)** (`:161-172`) — NVS dökümü alınırsa çevrimdışı kaba kuvvet hızlı; MAC tuzu cihaza özgüdür ama gizli değildir. Parola alt sınırı 4 karakter (`:417`).
6. **`/api/reset` anahtarı: 32 bit entropi, hız sınırı yok, maske 4 hex sızdırır** (`:1990-1992, 1935-1943, 2038-2043`). Uzaktan tahmin pratikte uzun sürer ama kilit/log yok; başarısız denemeler `sys_log`'a bile yazılmıyor.
7. **`/api/events` `Access-Control-Allow-Origin: *`** (`:1359`) — parola yokken herhangi bir site timer durumunu okuyabilir (bilgi sızıntısı, düşük).
8. **Logout `X-Auth-Token` ile açılan oturumu kapatamaz** (`:372` yalnızca çerez).
9. **`auth/status` oturumu uzatır** (`:320`) — GUI bunu periyodik çağırıyorsa "otomatik çıkış" hiç gerçekleşmez (§10).
10. **Factory reset dönüşleri denetlenmez** (`:1224-1227`); `nvs_flash_erase` başarısız olursa yine "ok" dönüp restart edilir.

### 8.2 Doğruluk / veri kaybı
11. **Kesilmiş gövde → sessiz veri silme** (`read_body` `:537` + cJSON başarısızlığı): `early-mail/save` 512+ baytlık gövde → üç alan **boş yazılır**, yanıt `ok` (`:1517-1524`). `trigger-mail/save` 1024+ bayt → `id` boş → `-1` → **yeni boş grup eklenir** (`:1605, 1624-1632`). `relay/set` 256+ → tüm değerler 0 kaydedilir. `auth/set` 384+ → hiçbir şey değişmez, `ok` döner. Üst sınır aşımında 413 yerine sessiz kırpma.
12. **`trigger-mail/delete` `id` eksikse grup 0 silinir** (`:1668-1669`: `atoi("")=0`, `save`'deki `strlen==0` kontrolü burada yok).
13. **Heap taşması `trigger-mail/files`** (`:2174-2197`): `int pos` + `2048 - pos` `size_t`'ye dönüşür; `pos > 2048` olunca negatif → devasa boyut → `snprintf` tampon dışına yazar. ~63 karakterlik adla ~23 dosya yeterli; upload'da dosya sayısı sınırı yok. Kimlik doğrulamalı ama bellek güvenliği hatası. `trigger-mail/get` aynı kalıbı kullanır (`:1546-1583`), hesapla 10 grup × en kötü escape ≈ 13.4 KB < 14 KB olduğundan taşmaz ama kırılgan (`cap - pos` wrap, `:1583` break sonrası yazım).
14. **`relay/test` gerçek tetikleme** (`:1284`, yorum "kisa pulse"): `duration=0` (süresiz) ayarlıysa röle kalıcı açılır; test için ayrı kısa darbe yok.
15. **`timer/set` `value` 1-60 kontrolü `unit`'ten bağımsız** ve `unit` geçersizse (İngilizce "day" gibi) yalnızca "Timer ayarlanamadi" (`:1023-1035`); API sözleşmesi Türkçe sözcüklere bağlı (`timer_manager.c:329-333`).
16. **`smtp/save` port aralığı yok**: `atoi` → `uint16_t` dönüşümü (`:1076`), "70000" → 4464.
17. **`wifi/connect` yanıtı/logu**: ssid escape edilmeden JSON'a gömülür (`:764`); `"` içeren SSID yanıtı bozar.
18. **`api-config/test` `read_body` dönüşü yok sayılır** (`:515`): gövde yoksa `body` başlatılmamış değil (`read_body` `buf[0]=0` yapar) — sorun yok; ama `target` eksikse sessizce webhook'a gider.

### 8.3 Yarış ve eşzamanlılık
19. **`s_sse_fds` kilitsiz paylaşım**: httpd görevi yazar (`:1349, 1364, 1381`), `sse_send` görevi okur/yazar (`:1407-1417`). `close_fn` slotu boşaltırken gönderim görevi aynı fd'ye `httpd_socket_send` yapabilir; fd yeniden kullanıldıysa veri yanlış sokete gider (close_fn yorumu bu riski bilir, `:1373-1375`, ama kilit yok).
20. **`s_sse_pending` ölü değişken** (`:48, 1428, 1438`): set/clear edilir, hiç okunmaz.
21. **SSE slot çalma indeks 0'ı kapatır** (`:1344`), en eskiyi değil.
22. **Handler'lar arası yarış yok** (tek httpd görevi) — ama bu, §6'daki bloklayan testlerin tüm API'yi durdurması demek. `sys_log_write`, `timer_manager_*`, `output_*` çağrıları diğer görevlerle (esp_timer, buton) paylaşılan durumdur; kilitleme modüllerin sorumluluğunda, burada doğrulanmadı.
23. **Yavaş istemci DoS**: `read_body` 5 × 5 s = 25 s'ye kadar tek görevi bloklar (`:540-545`); upload'da her parça için aynı (`:2126-2133`).

### 8.4 Bellek
24. **Yığın (8 KB httpd görevi)**: `api_apicfg_get_handler` `api_config_t c` (1672 B) + 7 escape tamponu (3250 B) = **~4.9 KB** yerel (`:444-452`); `api_apicfg_save_handler` `c`+`old` = 3344 B (`:484`); `api_early_mail_get_handler` ≈ 2.1 KB (`:1466, 1482, 1486`); `api_trigger_mail_get_handler` döngü içi ≈ 1.9 KB (`:1554-1555`); `api_telegram_get_handler` ≈ 1.6 KB (`:1841-1850`); `api_system_detail_handler` 1 KB + `system_info_t` (`:1130, 1142`). Üstüne `snprintf`/`cJSON`/`ESP_LOG` çerçeveleri. **SMTP testi** aynı yığında `mbedtls_ssl_handshake` çalıştırır — yığın yeterliliği doğrulanmamış (§10).
25. **Heap**: `trigger-mail/get` 14 KB (`:1538`), `api-config` GET 4 KB (`:446`), POST 2 KB (`:474`), `/api/logs` `sys_log_read_tail_json` dosyanın tamamını `malloc` eder (`sys_log.c:28`, boyut log döndürmesine bağlı), statik sunum 4 KB (`:564`). Sızıntı görülmedi: tüm `malloc`'lar her yolda `free` ediliyor.
26. **Her alan için cJSON parse** (§5) — geçici heap parçalanması.

### 8.5 Tekrarlayan kalıplar
27. `read_body(...) <= 0 → httpd_resp_send_err(400,"Veri alinamadi")` bloğu **17 kez** (`:340, 387, 479, 735, 777, 861, 891, 939, 1063, 1252, 1498, 1594, 1661, 1803, 1862, 1963, 2208`).
28. `snprintf(resp, …, "{\"status\":\"ok|error\",\"message\":\"…\"}")` + `send_json` kalıbı ~30 kez; ok/error için yardımcı yok.
29. "Gizli alan boşsa eskiyi koru" 3 kez (`:497, 1085-1090, 1876-1881`).
30. `wifi/connect` ve `wifi/backup/save` handler'ları satır satır kopya (`:732-771` vs `:774-813`).
31. `trigger-mail/delete` NVS kaydırma: 4 alan için aynı 6 satır kopyalanmış (`:1691-1717`); `gui-ota/info` iki özdeş `config.json` okuma bloğu (`:2249-2275`).
32. NVS aç/oku/kapat 11 handler'da satır içi (auth, early-mail ×2, trigger-mail ×3, actions ×2, reset ×4) — HTTP katmanı NVS şemasının sahibi.
33. `send_active_slot_file_chunked` öncesi `ext_flash_active_get_file_size` ile aynı dosya 2-3 kez aranıyor (`:651-657, 561`).

### 8.6 Hata yönetimi tutarsızlıkları
34. Aynı hata sınıfı (geçersiz değer) `relay/set`'te 400 text/html, `txpower`'da 200 JSON, `timer/set`'te hem 400 metin (`value`) hem 200 JSON (`unit`).
35. `reset/config` POST NVS hatasını yutar (`:1975-1979`), `reset/generate` döner.
36. Dispatcher 401 `error:"auth_required"`, login 401 `message:"Parola yanlis"`, auth/set 401 `message:"Oturum gerekli"` — üç ayrı 401 biçimi.
37. `logs/clear` `send_json` kullanmaz (`:1317-1321`).
38. HTTP durum kodları semantik değil: iş hataları 200, yalnızca gövde/parametre hataları 4xx.

---

## 9. Korunması gereken davranışlar

GUI de yeniden yazılacağı için yol ve alan adları serbestçe değişebilir; aşağıdaki liste **işlev** sözleşmesidir. Parantez içindekiler mevcut GUI'nin bugün bağımlı olduğu somut adlar (geçiş dönemi/uyumluluk katmanı için).

### 9.1 Var olması gereken işlevler
- Kimlik: durum sorgusu (`enabled`, `logged_in`), giriş/çıkış, parola koy/değiştir/kaldır (mevcut parola doğrulamalı), oturum süresi + kilit ayarları. Oturumsuz çağrıda GUI'nin "giriş ekranına dön" kararı verebileceği ayırt edici bir 401 gövdesi (`error:"auth_required"`).
- Cihaz: özet bilgi (ad, IP, AP SSID, mDNS, bağlı mı, auth açık mı), ayrıntılı bilgi (çip/heap/uptime/reset nedeni/flash/boot sayaçları/TX gücü), yeniden başlat, fabrika sıfırla, kalıcı olay logu oku/temizle.
- WiFi: birincil + yedek ağ (SSID, parola, isteğe bağlı statik IP/gw/mask/dns), sil, AP aç/kapa (STA bağlı değilken kapatılamaz), TX gücü tavanı + uyarlanabilir mod, durum (bağlı/IP/RSSI/AP istemci sayısı).
- Zamanlayıcı: durum (`enabled`, kalan/toplam sn, tatil kalan/toplam, alarm sayısı), ayarla (birim gün/saat/dakika × 1-60), iptal, yeniden başlat, tatil modu 1-60 gün başlat/iptal, alarm sayısı 0-10; **gerçek zamanlı durum akışı** (SSE veya eşdeğeri; GUI `EventSource('/api/events')`, `app.js:877`).
- Bildirim kanalları: SMTP (sunucu/port/kullanıcı/gizli; gizli hiç geri okunmaz, boş gönderilirse korunur; test), Telegram (token gizli/chat_id/mesaj şablonu; test), Webhook (URL/yöntem/başlıklar/gövde şablonu; test), MQTT (URL/topic/kullanıcı/parola gizli/payload şablonu; test). Şablon yer tutucuları `{status} {time} {device}` (`api_client.h:8-11`).
- Erken uyarı maili: alıcı/konu/gövde. Tetikleme mail grupları: en çok 10 grup (ad/konu/gövde/alıcılar), grup başına dosya ekleri (yükle ≤2 MB, listele, sil).
- Aksiyon anahtarları: erken uyarı için telegram/mail/api, tetikleme için mail/api/röle (varsayılan röle açık).
- Röle: süre 0-3600 (0 = süresiz), darbe 0-60, gecikme 0-60, ters mantık; test tetikleme.
- Uzaktan sıfırlama: anahtar üret (tek seferlik tam gösterim), aç/kapa, **kimlik doğrulamasız GET uç** (`/api/reset?key=`) — dış sistemlerin (cron, otomasyon) bağımlı olabileceği tek dış sözleşme; yol ve parametre adı **korunmalı** ya da sürümlü göç planlanmalı.
- OTA: firmware kontrol/indir/durum/rollback (resmi GitHub veya özel URL), GUI OTA başlat/durum/rollback (resmi veya özel repo/branch), ikisi için de `busy` ve ilerleme polling'i.
- Statik GUI: `/` kök, gzip, immutable önbellek + sürüm sorgusu, harici slot → gömülü sıralaması, dil dosyaları, logolar.

### 9.2 Yeni mimaride serbestçe değiştirilebilir
- Tüm `/api/...` yolları ve alan adları (tek dış bağımlılık `/api/reset?key=`).
- `timer/set`'in çok amaçlı `action` tasarımı ve Türkçe birim adları.
- Hata biçimi (tek tip JSON + anlamlı HTTP kodları + GUI'nin çevirebileceği hata kodları önerilir).
- Upload protokolü (özel `X-Group-Id`/`X-Filename` başlıkları yerine multipart veya yol parametresi).
- Ayar modelinin parça parça/kısmi-tam karışımı.
- `/ext/` statik öneki ve `ap_ip`, `slot_b_kb` gibi sabit kodlanmış değerler.

---

## 10. Açık sorular

1. **`auth_set_cookie` bozuk çerez** (§8.1-3): Cihazda login sonrası `Set-Cookie` başlığı gerçekten doğru mu geliyor? (Hafıza notu: canlı LAN testleri bekliyor.) Eğer çalışıyorsa yalnızca yığın düzeni sayesinde.
2. GUI `auth/status`'u periyodik çağırıyor mu? Evetse hareketsizlik TTL'i hiç işlemez (§8.1-9).
3. SMTP testi httpd görevinin 8 KB yığınında `mbedtls` el sıkışmasını güvenle tamamlıyor mu? Yığın izleme (`uxTaskGetStackHighWaterMark`) verisi yok.
4. `sys_log_read_tail_json` dosyanın tamamını heap'e alıyor; `events.log` üst boyutu/döndürme kuralı ne? (sys_log haritası başka ajanda.)
5. `wifi_manager_set_ap_enabled(false)` sonrası AP'de açık HTTP oturumları ne olur; `close_fn` SSE slotlarını temizliyor ama handler ortasında kesilen istekler?
6. `gui_ota_rollback` restart yapmıyor; tarayıcıdaki immutable önbellekli `app.js?v=X` ile yeni slotun `index.html`'i uyumsuz olabilir mi (sürüm sorgusu index'te değişir, ama `no-cache` index yeniden alınırsa sorun yok — GUI ajanıyla doğrulanmalı).
7. `/ext/*` beyaz listesine `/favicon.ico` dahil ama gömülü yok; tarayıcı her sayfada 404 alıyor — bilinçli mi?
8. `api_system_info` ve `system_detail` içindeki `slot_a_*` yalnızca Slot A'yı raporluyor; aktif slot B iken GUI'ye yanlış kapasite gösteriliyor olabilir (`:692-694, 1135-1137`).
9. Yeni mimaride HTTPS (self-signed) veya en azından AP'ye WPA2 parolası planlanıyor mu? §8.1-1 bunu zorunlu kılıyor.
10. `web_server_stop` hiç kullanılmıyor; yeni mimaride sunucu yaşam döngüsü (WiFi mod değişiminde yeniden başlatma) gerekli mi?
