# 03 — Araştırma özeti ve plana etkileri

Tarih: 2026-11-05. Girdi: `02-plan-ilk.md` §8'deki 15 soru; 7 araştırma belgesi `03-arastirma/01…07` (2 726 satır; kanıtlar IDF 5.5.2 `dosya:satır`, canlı ölçümler, resmî belgeler). Bu belge yalnız **plana etkisi olan** sonuçları toplar; ayrıntı ve kanıt alt belgelerde.

| # | Belge | Konu | Satır |
|---|---|---|---|
| 1 | `03-arastirma/01-http-sunucu.md` | httpd, SSE, CSRF, hız sınırı, cJSON, statik | 504 |
| 2 | `03-arastirma/02-tls-ve-kanallar.md` | esp-tls, SMTP STARTTLS, sağlayıcılar, Telegram, webhook, MQTT | 353 |
| 3 | `03-arastirma/03-ota-coredump-bolum.md` | OTA akışı, GitHub, rollback, coredump, bölüm tablosu | 370 |
| 4 | `03-arastirma/04-depolama.md` | NVS blob, harici flash sürücüsü, LittleFS, yedek, kademe-2 silme | 418 |
| 5 | `03-arastirma/05-wifi.md` | durum makinesi, tarama, reason kodları, TX gücü, SNTP, AP | 387 |
| 6 | `03-arastirma/06-derleme-test-gui-arac.md` | esbuild, CMake, gzip, i18n, SVG, Unity host, kapılar, build.sh | 352 |
| 7 | `03-arastirma/07-mimari-kaliplar.md` | aktif nesne, outbox politikası, zaman, röle/bekçi, bellek, güç, cfg şeması | 342 |

## 1. Plan doğrulandı (değişiklik yok)

- İki katman + üç görev (aktif nesne kalıbı) doğru; `esp_event` uygulama mantığı için uygun değil. (07)
- SSE için `httpd_req_async_handler_begin` + `httpd_queue_work`: LRU purge SSE'yi kapatmaz, her görevden güvenli. (01)
- SMTP STARTTLS **esp-tls ile mümkün** (`esp_tls_plain_tcp_connect` → `set_conn_sockfd` → `set_conn_state(CONNECTING)` → `conn_new_sync`); elle mbedTLS gerekmez. (02)
- NVS blob yazımı atomik (eski-ya-yeni); 64 KB yeterli (≈9/15 sayfa); `rt` ≤32 B ile aşınma onlarca yıl. (04)
- LittleFS `rename` atomik; `format_if_mount_failed=false` + elle format kodla doğrulandı. (04)
- GitHub API'siz OTA akışı kanıtlandı: `HEAD /releases/latest` → `Location` tag; `esp_https_ota_get_img_desc` flash'a dokunmadan sürüm kapısı. (03)
- Coredump 128 KB yeter; bölüm tablosu 4 MB'a tam oturur. (03)
- cJSON kalır (ek flash ≈ 0). (01) · Brotli düz HTTP'de çalışmaz → gzip. (06) · i18n `keys+dizi` −%29 ölçüldü. (06)
- Masaüstü Unity testleri çalışıyor (IDF içindeki Unity 2.6.0, shim yalnız `esp_log.h`). (06)

## 2. Plan düzeltmeleri (hatalı varsayımlar)

| Plan | Yanlış | Doğru | Kaynak |
|---|---|---|---|
| §5.4 | "reason 2/8 = biz kopardık, yeniden deneme yok" | 2 = `AUTH_EXPIRE` (gerçek hata), 3/8 router'dan da gelir. Kendi kopmamız `self_disc` bayrağı + `DISCONNECTING` durumu ile tanınır; tüm dış kopmalar yeniden denenir | 05 |
| §5.6 | "abort'ta otadata değişmez" | `esp_ota_begin` pasif otadata girdisini siler; abort sonrası geri dönüş doğru olarak kaybolur; her istekte `esp_ota_check_rollback_is_possible()` | 03 |
| §5.6 | GitHub Releases ile semver | `smrtkrft/LebensSpur` `releases/latest` = `DMF_v1.0.6` (başka ürün) → **ayrı depo ya da manifest** zorunlu; `project_name` kapısı zorunlu | 03 |
| §5.6 | ALTE OTA referans alınabilir | ALTE'nin GitHub OTA'sı bugün çalışmaz: imzalı URL 883 B, `buffer_size_tx` 512 → "Out of buffer"; 2611'de **2048** | 03 |
| §5.7 | `Sec-Fetch-Site` alternatif | düz HTTP'de hiç gönderilmez → kullanılmaz | 01 |
| §6.1 | mutex'li SSE istemci listesi | tüm erişim httpd görevinde → kilitsiz; tek atomik `pending` | 01 |
| §6.2 | IDF rotasına yol parametresi | IDF desteklemez → yöntem başına tek `/*` + kendi rota tablosu | 01 |
| §6.4 | hash'li dosya adı (`app.<hash>.js`) | CMake `OUTPUT` ile çelişir → sabit ad, hash URL yolunda `/s/<hash>/app.js`, `gui_hash.txt` gömülür | 06 |
| §3.4 | röle güvenli seviyesi `cfg_load` sonrası | GPIO19 ROM/bootloader boyunca yüksek empedans; `gpio_reset_pin` pull-up açar → yasak; pin NVS'ten **önce** varsayılan polariteyle sürülür, `inverted` için **donanım pull direnci şart** (üretim notu + GUI uyarısı) | 07 |
| §4.1 | `EV_TICK` sayaç azaltır | **son tarih aritmetiği**: `remaining = deadline − now`; tik yalnız değerlendirir; tik düşmesi/blokaj sayımı bozmaz | 07 |
| §5.5 | "doğrulama kapalı" basit bayrak | esp-tls doğrulamasız bağlantıyı yalnız `CONFIG_ESP_TLS_INSECURE=y` + `SKIP_SERVER_CERT_VERIFY=y` ile kabul eder | 02 |
| §5.2 | Winbond çip 32 MB | C6'da Winbond sürücüsü kapalı → W25Q256 16 MB'a kırpılır; `CONFIG_SPI_FLASH_SUPPORT_WINBOND_CHIP=y` (bench'teki "16 MB" okuması muhtemelen bu) | 04 |
| §5.8 | `phy_init` bölümü | gereksiz (kalibrasyon NVS'te) → kaldırılır, 20 KB `nvs_keys` için ayrılır | 03 |
| §3.4 | eski NVS otomatik silinir | 64 KB bölüm ALTE'nin `nvs/otadata/phy` alanını kapsar, `init` ESP_OK döner ve ALTE anahtarları görünür kalır → "`meta` yoksa `nvs_flash_erase`" kodla zorunlu; tezgâhta tek seferlik `erase-flash` | 03, 04 |

## 3. Plan iyileştirmeleri (kabul edilenler)

### Çekirdek (§3–4)
- `app_core` "asla bloklamaz" → "flash dışında bloklamaz": `rt` blob yazımı `app_core`'da kalır (C6'da flash yazımı hangi görevden olursa olsun sistemi durdurur), yalnız değişince yazılır, süresi ölçülür (>200 ms evlog). Kuyruk-dolu politikası: ISR/timer 0 bekleme + `q_drop` sayacı; httpd ≤100 ms → `503 busy`. (07)
- `rt` blob: `trigger_seq`, `since_trigger_s`, `attempts[]`, `early_pending`; `deadline` asla yazılmaz. (07)
- Bekçi: `app_core` ve `sys_worker` TWDT abonesi; `delivery` için `app_core` iş başına son tarih tutar, aşımda `shutdown(sock)` → `sys_restart(RST_DELIVERY_HANG)`. (07)
- Düğme penceresi ve `relay_sm` de son tarih aritmetiğiyle (`now_fn` enjekte, host testi). (07)
- `app/core` CMake `REQUIRES` beyaz listesi (IDF bağımsızlık derlemede zorlanır). (07)

### Teslimat ve kanallar (§4.6–4.7)
- Sonuç sınıfları `OK / RETRY / PERM / CONFIG` (`CONFIG` deneme harcamaz, GUI'ye düşer). (02)
- SMTP: EHLO ayrıştırma (AUTH `PLAIN`/`LOGIN` ikisi de; SIZE; 8BITMIME), çok satırlı `250-`, kod tablosu, **dot-stuffing**, CRLF normalizasyonu, RFC 2047 konu, `Date`/`Message-ID`, `EHLO [ip]`, base64 76/satır, parça **2964 B ham → 4056 B** (57'nin katı, tek TLS kaydı), AUTH satırları logda maskeli; `pending` biti yalnız `DATA 250`'de temizlenir; sabit `Message-ID: <ls-{seq}-g{grup}@host>` (yeniden gönderimde tekilleştirme). (02, 07)
- Sağlayıcı gerçeği (GUI yardım metni): Gmail ve iCloud uygulama parolası ile çalışır; **Outlook.com kişisel hesap parola ile çalışmaz** (temel kimlik doğrulama kapalı); M365 iş 2026 sonuna kadar. (02)
- Telegram/webhook: `parse_mode` yok, yanıt gövdesi okunur (`retry_after`), `buffer_size_tx=1024`, `max_redirection_count=2`, `Connection: close`; `esp_http_client_close` başka görevden **çağrılmaz**. (02)
- Webhook `Idempotency-Key`/`event_id`, MQTT `seq`. (07)
- `EV_NET_UP` + 10 s yerleşme; röle enerjilenme ile TLS bağlantısı aynı saniyede değil (`power_quiet_until`); OTA sırasında teslimat önceliği (`EV_OTA_BUSY`). (07)
- TLS 1.2 kalır; tek oturum tepe heap ≈ 50 KB; `delivery` 10 KB yığın yeterli, parça tamponları heap'te. (02)

### Platform (§5)
- **cfg**: X-macro yerine `cfg_schema.json` + `tools/gen_cfg.py` → `cfg_schema.gen.h/.c` + `GUI/src/schema.gen.json` (ilke 4 GUI'de de); blob başına `{schema,len}` başlığı; **gruplar `cfg_t` dışında** (`cfg_group_get`, 27,6 KB statik RAM tasarrufu); `_Static_assert(blob ≤ 4000 B)`; `nvs_get_stats` → `/api/storage`. (04, 07)
- **Yedek**: tek büyük JSON yerine **bölüm başına satır JSONL + CRC32** (≤12 KB heap yerine ≈100 KB); `config.tmp → prev → cur` rename zinciri; `export/restore` akışlı; `import` GUI'de bölüm bölüm `PUT`; yazım 300 ms birleştirme; ileride şifreleme için `cipher` kancası. (04)
- **Storage**: durum makinesi `ABSENT/MOUNTED/ERROR/FORMATTING` + okuma sayacı/dışlayıcı kilit (`esp_littlefs_format` açık fd'leri sessizce boşaltır); `.ls2611` düzen işareti; boş bölge 8 KB denetimi; SPI2 `SPI_DMA_DISABLED`, DIO 40 MHz; boyut `[4,32] MB` denetimi; içerik `fsync`/`close` olmadan kalıcı değil. (04)
- **Kademe-2 sıfırlama** = `esp_partition_erase_range` ile tam silme (16 MB ≈ 38 s; `lfs_format` sırları silmez) + biçimleme; iş kimliği + ilerleme; `factory2_pending` ile kesintiye dayanıklı. (04)
- **evlog**: `/log/events.jsonl`; boşaltma aç-yaz-`fsync`-kapat; döndürme `EBUSY`'de ertelenir; okuma httpd'de kısa aç-oku-kapat; açılışta coredump özeti → boot kaydı; reset nedenlerine `RST_DELIVERY_HANG`; rollback olduysa `esp_ota_get_last_invalid_partition` → evlog. (03, 04, 07)
- **WiFi**: `DISCONNECTING` durumu; 1..5 s denemeler `CONNECTING` içinde; tarama 0/120 ms, `home_chan_dwell_time` istemci varken 100 ms; aralık ilk 2 dk 15 s → sonra 60 s, istemci varken ≥60 s; kör deneme yerine `hidden` bayrağı (GUI onay kutusu); bağlanırken `sta.channel` ipucu + `WIFI_FAST_SCAN`; 30 dk birincil denemesi koparmadan arka plan taramasıyla (RSSI ≥ −75); `STA_LOST_IP` (120 s gecikmeli) → `self_disc` + yeniden bağlanma; hostname `esp_wifi_start` öncesi iki netif'e; SNTP `wait_for_sync=false`; AP `channel=1`, `esp_wifi_set_inactive_time(AP,120)`; `ap.pass` şemada (boş = açık); modem uykusu APSTA'da etkisiz (sabit, ayar değil). (05)
- **TX gücü**: config `tx_max` 0,25 dBm kayıt değeri (28…80, varsayılan 66 = 16,5 dBm); GUI **9 gerçek kademe**; RSSI tablosu 28/44/56/66; AP istemcisinde ≤56; `CONFIG_ESP_PHY_REDUCE_TX_POWER=y` (brownout sonrası açılışta PHY tavanı 13 dBm). (05, 07)
- **OTA**: `HEAD /releases/latest` → tag; indirme `esp_https_ota_get_img_desc` → `project_name` + `semver_cmp > 0` kapısı; `buffer_size_tx=2048`; `partial_http_download` kapalı; `timeout_ms` 10 s; `PENDING_VERIFY` iken OTA/geri dönüş reddi; onay işi `sys_worker`'da; `sys_worker` yığını **12 KB**; iptal `atomic_bool`; release'e `.elf` eklenir (coredump çözümleme masaüstünde). (03)
- **Coredump**: ELF + CRC32, `STACK_SIZE=1792`, `LOGS=n`, `MAX_TASKS_NUM=24`; +12–18 KB flash (ölçülecek). (03)

### HTTP ve GUI (§6)
- Statik istek bağlamı `s_ctx` (gövde 2 KB, yanıt 4 KB+5, çerez/ETag dizileri, IP) — W1 yapısal çözüm; heap yalnız cJSON ağacı. (01)
- CSRF: `Host` allowlist + `X-LS-Request: 1` zorunlu + `Origin == http://Host` (varsa; `null` red) + `Content-Type: application/json` + `SameSite=Strict`. (01)
- Giriş kilidi / hız sınırı: IP başına 8 girişli LRU tablo (`getpeername`, `sockaddr_in`) + global arka durdurucu. (01)
- httpd: öncelik 5, 8 KB, `max_open_sockets` 10, `lru_purge` açık, `max_uri_handlers` 8, `max_resp_headers` 10, 5/5 s, keepalive kapalı; `CONFIG_LWIP_MAX_SOCKETS=20`, `HTTPD_MAX_REQ_HDR_LEN=2048`, `HTTPD_MAX_URI_LEN=256`, `-DCJSON_NESTING_LIMIT=16` (**zorunlu**: 2 KB `[[[[…` 8 KB yığını taşırır). (01)
- SSE: 4 slot, `sess_ctx + free_ctx`, canlılık `recv(MSG_PEEK)`, kalp atışı durum anlık görüntüsü taşır, tüm gönderim `httpd_resp_send_chunk`. (01)
- Statik: gömülü gzip tek `httpd_resp_send`; chunked yalnız LittleFS; `ETag`/304 yalnız `index.html`; `Vary: Accept-Encoding`. (01, 06)
- GUI boyut: logo **elle çizilmiş SVG** (280 B; PNG'ler kalkar, −21 KB gz — en büyük tek kazanç); sprite satır içi (1,8 KB); favicon SVG + 32×32 PNG yedek (Safari ≤18); i18n bütçe sınırda → indeks modu (`t(123)`) yedek plan; sekiz i18n doğrulama kuralı. (06)
- `/api/wifi`: `last_error {class, reason, rssi}` (sınıflar: `auth`, `not_found`, `security`, `other`), `tx_now` (0,25 dBm), `ap_auto_opened`. `/api/ota`: `pending_version`, `rollback_available`, `rollback_version`, `last_failed_version`. `/api/device`: `heap_largest`. `/api/config/import` ucu kalkar (GUI bölüm bölüm `PUT`). (01, 03, 05, 07)

### Derleme ve sınama (§6.4–6.5)
- `build.py` Python 3.9 + stdlib (IDF venv), pip yok; esbuild `npx --yes --offline esbuild@0.28.2`; depoda `package.json`/`node_modules` yok; gzip -9 `mtime=0`. (06)
- CMake `add_custom_command` + `GLOB_RECURSE CONFIGURE_DEPENDS` (kanıtlı); çıktı `<build>/esp-idf/http/gui/`. (06)
- `esp_bin/test.sh`: `export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"` (bu Mac'te CLT 27.2 SDK + Xcode `ld` uyumsuzluğu), `ctest --output-on-failure`, isteğe bağlı `-DSAN=ON`. (06)
- `-Werror` IDF'te zaten küresel; proje bileşenlerine `cmake/warnings.cmake`. İmaj eşiği 1,5 MiB `build.sh`'de + `esp_idf_size` JSON günlüğü. cppcheck `brew` ile. (06)
- `build.sh`: `--alte` ilk bayrak, `test` alt komutu, `idf.py -C`, firmware adı ALTE için `alte_` ön eki. (06)
- Adım 0'da heap ölçüm tablosu (`heap_caps_get_info`, en büyük blok); adım 1'de grup blob yazım süresi, `/data` mount hata senaryoları, çip yok senaryosu. (04, 07)
- Tezgâh ölçümleri (denetim listesine): tarama sırasında telefon kopması (dwell 30 vs 100), AP→APSTA'da `AP_STOP/START`, WPA3 yanlış parola reason'ı, beş uç için DNS/TCP/TLS süreleri, `heap_min`, router listesinde hostname, SSE slot boşalması, `[[[[` gövdesi, `Host: evil.com` → 403. (01, 02, 05)

### sdkconfig eklemeleri (ALTE seti + aşağıdakiler)
`ESP_WIFI_NVS_ENABLED=n`, `ESP_PHY_REDUCE_TX_POWER=y`, `ESP_PHY_MAX_WIFI_TX_POWER=17`, `ESP_WIFI_ENABLE_SAE_PK=n`, `ESP_WIFI_SOFTAP_SAE_SUPPORT=n`, `ESP_WIFI_ENABLE_WPA3_OWE_STA=n`, `ESP_WIFI_DYNAMIC_RX/TX_BUFFER_NUM=16`, `SPI_FLASH_SUPPORT_WINBOND_CHIP=y`, `SPI_FLASH_AUTO_SUSPEND=n`, `LITTLEFS_MAX_PARTITIONS=2`, `LITTLEFS_LOOKAHEAD_SIZE=512`, `LWIP_MAX_SOCKETS=20`, `HTTPD_MAX_REQ_HDR_LEN=2048`, `HTTPD_MAX_URI_LEN=256`, `HTTPD_WS_SUPPORT=n`, `HTTPD_QUEUE_WORK_BLOCKING=n`, `MBEDTLS_DYNAMIC_BUFFER=n`, `PM_ENABLE=n`, `ESP_COREDUMP_ENABLE_TO_FLASH=y` + ELF/CRC32 seti, `ESP_TLS_INSECURE=y` + `ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` (yalnız SMTP kullanıcı seçimi için), `ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096`.

## 4. Kullanıcı kararı gerektirenler

| # | Konu | Seçenekler | Öneri |
|---|---|---|---|
| K1 | OTA deposu: `smrtkrft/LebensSpur` `releases/latest` başka ürünü (DMF) gösteriyor | (a) 2611 için ayrı GitHub deposu; (b) aynı depoda `lebensspur-2611.json` manifest | **a** — API'siz akış yalnız ayrı depoda çalışır; her durumda `project_name` kapısı |
| K2 | Harici flash bölümleri | (a) `/log` 1 MB + `/data` kalan; (b) tek `/data` | **a** — günlük otomatik onarım, kademe-2'de hayatta kalır (ilke 11), ek yazımı günlüğü bozamaz |
| K3 | MQTT istemcisi | (a) `net_tls` üzerinde ~150 satır MQTT 3.1.1 tek-yayın; (b) esp-mqtt (görev yaratır, 6 KB) | **a** — −14 KB flash, "görev yaratılmaz" kuralı korunur; esp-mqtt yedek |
| K4 | Teslimat yeniden deneme | (a) 0/1/5/15/30 dk → 30 dk'da bir ±%10 jitter, **72 saat** açık süre ya da erteleme; EARLY 3 deneme 0/1/5 dk; (b) plandaki 24 saat, jitter yok, EARLY 2 deneme | **a** — RFC 5321 §4.5.4 ve MTA uygulamaları; greylisting |
| K5 | Brownout sonrası açılış | (a) TX tavanı 8 dBm + teslimat 30 s ertelenir, **içerik değişmez**; (b) a + ekler atlanır; (c) hiçbir özel davranış | **a** — brownout-reboot döngüsü kırılır, teslimat içeriği korunur |

## 5. `yapilacaklar.md`'ye eklenecekler

- SMTP özel CA sertifikası (PEM) desteği; XOAUTH2 / Device Code (Outlook.com kişisel hesaplar).
- NVS şifreleme + yedek şifreleme (HMAC türevli AES-GCM, eFuse blok planı); 20 KB `nvs_keys` boşluğu ayrıldı.
- AP istemcileri için minimal DNS yanıtlayıcı (yalnız kendi adı); RSSI'ye göre birincil/yedek seçimi; AP kapalıyken `MIN_MODEM`.
- clang-tidy (`esp-clang`); W25Q256 32 MB doğrulaması; `/log` bölüm boyutu gözden geçirme.
- Uyku modu açılacaksa harici 32 kHz kristal; ek içeriği değişen yeniden gönderimde `Message-ID` eki.
