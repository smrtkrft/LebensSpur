# 02 — Ağ ve dış iletişim haritası

Kaynak: `ALTE/HW` (v0.0.70, ESP-IDF 5.5.2, hedef esp32c6). Tarih: 2026-11-05.

Kapsam: `wifi_manager`, `smtp_client`, `telegram_client`, `api_client` bileşenleri; `main/idf_component.yml`; `sdkconfig.defaults` dosyasının ağ/TLS/lwIP/mbedTLS/mDNS/MQTT kısımları. `main.c`, `web_server.c`, `timer_manager.c`, `sys_log.c`, `ls_util.c` yalnızca bu modüllerin nasıl çağrıldığını anlamak için okundu.

Satır referansları `dosya:satır` biçimindedir; dosya adı tek başına yazıldığında yol `ALTE/HW/components/<bileşen>/` altındadır (`main.c` → `ALTE/HW/main/main.c`, `web_server.c` → `ALTE/HW/components/web_server/web_server.c`). Üretilen `esp_bin/HW/sdkconfig` değerleri "etkin değer" olarak ayrıca belirtilir. Cihazda sınanmamış, yalnızca koddan çıkarılan iddialar **(koddan çıkarım)** etiketiyle işaretlendi.

---

## 0. Genel bakış

### 0.1 Modüller ve ilişkiler

```
                     ┌──────────────┐
   timer_manager ───►│   main.c     │──── xTaskCreate ──► alarm_mail / alarm_tg / alarm_api
   (esp_timer cb)    │ (orkestra)   │                      trig_mail / trig_api
                     └──────┬───────┘
                            │ init sırası: smtp → telegram → api → wifi → web_server → auto_connect
          ┌─────────────────┼──────────────────┬──────────────────┐
          ▼                 ▼                  ▼                  ▼
   wifi_manager        smtp_client      telegram_client       api_client
   esp_wifi/esp_netif  lwIP soket +     esp_http_client +     esp_http_client (webhook)
   mdns (yönetilen)    mbedTLS (elle)   esp-tls (bundle)      esp-mqtt (MQTT)
   esp_netif_sntp      esp_crt_bundle                         cJSON (başlıklar)
          ▲                 ▲                  ▲                  ▲
          └────────── web_server.c (GUI REST uçları: ayar/test) ──┘
```

- `wifi_manager` radyonun tek sahibi değildir: `system_info.c:63-66` RSSI'yi doğrudan `esp_wifi_sta_get_ap_info` ile okur.
- `sys_log` bütün modüllerden çağrılır (RTC olay işareti + harici flash'a JSONL satırı).
- `api_client` → `wifi_manager_get_device_name()` bağımlılığı vardır (`api_client.c:113,152,232`).

### 0.2 Görev / bağlam haritası

| Çağrı | Görev (bağlam) | Yığın | Öncelik | Kaynak |
|---|---|---|---|---|
| `wifi_manager_init`, `wifi_manager_auto_connect` | `main` (app_main) | 3584 (etkin `CONFIG_ESP_MAIN_TASK_STACK_SIZE`) | 1 | `main.c:487,527` |
| `wifi_event_handler` | `sys_evt` (varsayılan olay döngüsü) | 4096 (`sdkconfig.defaults:47`) | 20 | `wifi_manager.c:583-586` |
| `reconnect_task_fn` | `wifi_reconn` | 4096 | 2 | `wifi_manager.c:394` |
| `tx_power_evaluate`, `sta_retry_cb` | `esp_timer` | 3584 (etkin) | 22 | `wifi_manager.c:589-594` |
| `wifi_manager_connect_sta/save_backup/set_ap_enabled/set_tx_power/clear_*` | `httpd` | 8192 (`web_server.c:2613`) | 5 (IDF varsayılanı) | `web_server.c:756-902` |
| `smtp_client_send` (erken uyarı) | `alarm_mail` | 10240 | 5 | `main.c:218` |
| `smtp_client_send[_with_attachments]` (tetikleme) | `trig_mail` | 10240 | 5 | `main.c:358` |
| `smtp_client_send_test` | `httpd` | 8192 | 5 | `web_server.c:1114` |
| `telegram_client_send` (erken uyarı) | `alarm_tg` | 8192 | 5 | `main.c:220` |
| `telegram_client_send` (test) | `httpd` | 8192 | 5 | `web_server.c:1905` |
| `api_client_notify_all` | `alarm_api` / `trig_api` | 8192 | 5 | `main.c:223,362` |
| `api_client_send_webhook/mqtt` (test) | `httpd` | 8192 | 5 | `web_server.c:521-524` |
| `on_alarm_triggered`, `on_timer_expired` (görevleri yaratan) | `esp_timer` | 3584 | 22 | `timer_manager.c:218,227` → `main.c:212,342` |

### 0.3 Sabit kodlanmış zaman aşımları ve sayılar (toplu)

| Değer | Nerede | Anlamı |
|---|---|---|
| 5 / 1000 ms / 15000 ms | `wifi_manager.c:65-67` | STA retry sayısı, retry taban gecikmesi (1,2,3,4,5 s), periyodik tarama aralığı |
| 200 ms, 500 ms | `wifi_manager.c:282,686` | disconnect sonrası bekleme; auto_connect öncesi bekleme (vTaskDelay) |
| 120–500 ms/kanal | `wifi_manager.c:312-313` | aktif tarama süresi; 13 kanalda ≈ 1,6–6,5 s engelleyici |
| 10000 / 2500 ms / 2 | `wifi_manager.c:76-78` | TX değerlendirme periyodu, olay sonrası hızlı değerlendirme, düşürme histerezisi |
| 8 / 20 / 17 / 8 dBm | `wifi_manager.h:10-13` | TX min, max, varsayılan tavan, açılış gücü |
| 14 dBm | `wifi_manager.c:445-446` | AP'ye istemci bağlanınca anlık tavan |
| -95 dBm | `wifi_manager.c:275` | STA bağlanma RSSI eşiği |
| 4 istemci, kanal 0 | `wifi_manager.h:6-7` | AP |
| "192.168.4.1" | `wifi_manager.c:46,615`; `web_server.c:704` | AP IP'si (IDF DHCP sunucu varsayılanı; iki yerde metin olarak) |
| "8.8.8.8" | `wifi_manager.c:210` | statik IP'de yedek DNS |
| "pool.ntp.org" | `wifi_manager.c:949` | SNTP sunucusu; güncelleme 1 saat (etkin `CONFIG_LWIP_SNTP_UPDATE_DELAY=3600000`) |
| 15 s | `smtp_client.c:259` | SMTP soket okuma/yazma zaman aşımı (her komut için ayrı) |
| 465 | `smtp_client.c:287,573` | bu port → doğrudan TLS; diğer her port → STARTTLS |
| 1024 B | `smtp_client.c:28` | SMTP tampon (başlık+gövde tek `snprintf`) |
| 570 / 800 B | `smtp_client.c:710-711` | ek dosya base64 parça tamponları (yığında) |
| 10000 ms | `telegram_client.c:114`; `api_client.c:160` | HTTPS istek zaman aşımı |
| 3 | `api_client.c:162` | webhook yönlendirme sayısı |
| 8000 / 8000 / 5000 ms | `api_client.c:242,253,260` | MQTT ağ zaman aşımı, bağlanma bekleme, yayın onayı bekleme |
| QoS 1, retain 0 | `api_client.c:255` | MQTT yayın |
| 1700000000 | `api_client.c:107` | "saat senkron mu" eşiği (2023-11-14); altındaysa `{time}` = çalışma süresi |
| 5 dosya, 2 MB/dosya, 10 grup | `main.c:276`; `web_server.c:2089`; `web_server.c:1626` | tetikleme maili ekleri |
| 60 s | `main.c:38` | OTA onayı (ağ dışı ama açılış akışında) |

---

## 1. `wifi_manager`

Dosyalar: `wifi_manager.c` (955 satır), `include/wifi_manager.h` (111 satır), `CMakeLists.txt`.

### 1.1 Modül amacı

Cihazın tek radyo yöneticisi. Açılışta parolasız bir AP açar (SSID = cihaz adı `LS-XXXXXXXXXX`), ardından kayıtlı birincil/yedek ağa tarama yaparak bağlanır (APSTA). Kopmada zamanlayıcı tabanlı yeniden deneme, 15 saniyede bir sonsuz tarama; statik IP; mDNS duyurusu; SNTP; besleme kaynaklı brownout'ları önlemek için TX gücü yönetimi (yumuşak başlangıç + uyarlanabilir güç). Tasarım özeti dosya başındaki yorumdadır (`wifi_manager.c:1-23`).

"Üç kademeli WiFi altyapısı" (README) kodda şu üç katmana karşılık gelir: (1) birincil STA ağı `p_*`, (2) yedek STA ağı `b_*`, (3) her zaman açık AP. **AP'ye "düşme" diye ayrı bir geçiş yoktur**: AP açılışta başlar ve kullanıcı kapatmadıkça hep açık kalır (`wifi_manager.c:12,52,597`).

### 1.2 Dış arayüz

Tüm fonksiyonlar `wifi_manager.h` içinde. "Engelleyici" sütunu çağıran görevi ne kadar bekletir sorusuna cevaptır.

| Fonksiyon | Ne yapar | Dönüş / hata sözleşmesi | Engelleyici | Çağıran |
|---|---|---|---|---|
| `wifi_manager_init(const char *device_name)` `c:550-598` | NVS'den AP/TX ayarlarını ve kimlikleri yükler; netif, olay döngüsü, `esp_wifi_init`, olay kayıtları, 3 esp_timer; mDNS; AP'yi başlatır | `wifi_manager_start_ap()` sonucu (her zaman `ESP_OK`); içerideki `ESP_ERROR_CHECK`'ler (`c:575-586,605-607`) hata durumunda **abort/panik** | Hayır (ms mertebesi) | `main.c:487` (app_main) |
| `wifi_manager_start_ap(void)` `c:600-619` | Mod = AP, açık SSID, `esp_wifi_start`, TX 8 dBm, periyodik TX zamanlayıcısı, `s_ip_addr="192.168.4.1"` | `ESP_OK` | Hayır | yalnızca `init` |
| `wifi_manager_connect_sta(ssid, password, st)` `c:631-677` | Birincil kimliği değiştirir, NVS'ye yazar (sonuçtan bağımsız), reconnect görevini öldürür, APSTA'ya geçer, bağlantıyı **başlatır** | `ESP_ERR_INVALID_ARG` (ssid boş); aksi `ESP_OK` — bağlantı sonucu **bildirilmez**, asenkron | Evet: `do_connect` içinde 200 ms `vTaskDelay` (`c:282`) | `web_server.c:760` (httpd) |
| `wifi_manager_auto_connect(void)` `c:679-689` | Kayıtlı ağ varsa APSTA'ya geç, 500 ms bekle, tara ve en iyi ağa bağlan | `ESP_ERR_NOT_FOUND` (kayıt yok, AP'de kal); `ESP_OK` (deneme başlatıldı) | Evet: 500 ms + engelleyici tarama (≈1,6–6,5 s) + 200 ms | `main.c:527` (app_main sonu) |
| `wifi_manager_save_backup(ssid, password, st)` `c:700-727` | Yedek kimliği NVS'ye yazar; **bağlantı denemez** | `ESP_ERR_INVALID_ARG`; `nvs_open` hatası; `ESP_OK` | Hayır | `web_server.c:802` |
| `wifi_manager_set_ap_enabled(bool)` `c:732-767` | AP'yi açar/kapatır, NVS'ye yazar | `ESP_ERR_INVALID_STATE` (STA bağlı değilken kapatma reddi `c:735-738`); `ESP_OK` | Hayır | `web_server.c:870` |
| `wifi_manager_set_tx_power(int max_dbm, bool adaptive)` `c:885-904` | Tavanı/uyarlanabilir modu ayarlar, hemen değerlendirir, sys_log'a yazar, NVS'ye yazar | `ESP_ERR_INVALID_ARG` (8..20 dışı); `ESP_OK` | Hayır (sys_log_write flash yazar) | `web_server.c:902` |
| `wifi_manager_clear_primary/clear_backup` `c:771-796` | NVS anahtarlarını siler, RAM'i sıfırlar; **mevcut bağlantıyı koparmaz** | `nvs_open` hatası ya da `ESP_OK` | Hayır | `web_server.c:756,798` |
| `wifi_manager_has_saved_config / has_backup_config` `c:691,729` | birincil/yedek SSID dolu mu | bool | Hayır | — (web_server yalnızca `has_backup_config` kullanır) |
| `wifi_manager_is_connected` `c:692` | STA IP aldı mı (`s_is_connected`) | bool | Hayır | web_server, api_client dolaylı |
| `wifi_manager_get_ip` `c:693` | Son STA IP'si; bağlı değilken AP IP'si ya da **eski STA IP'si** (bkz. 1.8) | `const char*` | Hayır | `web_server.c:716,841` |
| `wifi_manager_get_device_name / get_ap_ssid` `c:694-695` | `LS-…` adı (ikisi aynı) | `const char*` | Hayır | web_server, api_client |
| `wifi_manager_get_sta_ssid` `c:696` | Aktif STA SSID'si; yoksa kayıtlı birincil | `const char*` | Hayır | `web_server.c:718,821` |
| `wifi_manager_get_backup_ssid` `c:730` | yedek SSID | `const char*` | Hayır | `web_server.c:822` |
| `wifi_manager_get_primary_static / get_backup_static` `c:697-698` | Statik IP yapıları (GUI ön-doldurma) | `const wifi_static_cfg_t*` | Hayır | `web_server.c:818-819` |
| `wifi_manager_is_ap_enabled` `c:769` | AP ayarı | bool | Hayır | web_server |
| `wifi_manager_get_tx_power_max / _now / get_tx_adaptive / get_ap_client_count` `c:906-909` | TX ve istemci durumu | int/bool | Hayır | `web_server.c:848-852,1195-1197` |
| `wifi_manager_get_sta_rssi` `c:911-916` | Bağlıysa router RSSI, değilse 0 | int | Hayır | web_server, sys_log bağlamı |

`wifi_static_cfg_t` (`h:17-22`): `ip[16] gw[16] mask[16] dns[16]`; `ip` boşsa DHCP.

### 1.3 Davranış

#### Açılış sırası (`main.c:426-528` ile birlikte)

1. `wifi_manager_init` (`main.c:487`): NVS → `ap_enabled`, `tx_max`, `tx_adapt` (`c:556-569`); kimlikler + eski anahtar taşıma (`c:139-173`); `esp_netif_init`, varsayılan olay döngüsü, AP ve STA netif'leri, `esp_wifi_init` (`c:575-581`); olay kayıtları: tüm `WIFI_EVENT` ve yalnız `IP_EVENT_STA_GOT_IP` (`c:583-586`; `STA_LOST_IP` işlenmez); üç esp_timer (`sta_retry`, `tx_eval`, `tx_kick`) (`c:589-594`); mDNS (`c:596`, WiFi başlamadan önce); AP başlat (`c:597`).
2. `wifi_manager_start_ap` (`c:600-619`): mod `WIFI_MODE_AP`, `WIFI_AUTH_OPEN` (`c:224`), kanal 0 (otomatik; APSTA'da STA kanalına uyar), 4 istemci; `esp_wifi_start`; TX 8 dBm (`c:611`); 10 s periyodik TX zamanlayıcısı başlar; 2,5 s sonra tek seferlik "kick" (`c:612-613`).
3. `web_server_start` (`main.c:494`) — AP üzerinden GUI, `auto_connect`'ten önce hazır olsun diye.
4. `wifi_manager_auto_connect` (`main.c:527`): kayıt yoksa AP'de kalır (`ESP_ERR_NOT_FOUND`); varsa `ensure_sta_mode` (AP→APSTA, `c:236-240`), 500 ms bekleme, `scan_and_connect`.

#### Durum makinesi: STA bağlantı

Durum değişkenleri: `s_is_connected`, `s_retry_count`, `s_trying_backup`, `s_reconnect_task`, `s_retry_exhausted_logged` (`c:45-73`).

**Tarama ve seçim** `scan_and_connect` (`c:299-365`):
- Aktif tarama, gizli ağlar dahil, kanal başına 120–500 ms (`c:309-314`), **engelleyici** (`esp_wifi_scan_start(&cfg, true)` `c:317`).
- En fazla 20 kayıt `malloc` ile alınır (`c:327-328`); SSID **tam eşleşme** (`strcmp`) aranır (`c:334-335`).
- Öncelik: birincil bulundu → birincil; yalnız yedek bulundu → yedek; hiçbiri yok ve birincil kayıtlı → birincil "gizli ağ olabilir" diye kör deneme; aksi yedek kör deneme (`c:348-364`). Not: her iki ağ da görünürken RSSI karşılaştırması **yapılmaz**, birincil kazanır.

**Bağlanma** `do_connect` (`c:279-295`): `esp_wifi_disconnect` + 200 ms `vTaskDelay`; STA config: tüm kanal tarama, sinyale göre sırala, auth eşiği parola varsa `WPA2_PSK` yoksa `OPEN`, RSSI eşiği -95 (`c:266-276`); statik IP uygula (`apply_static_ip`); `s_retry_count=0`; `SL_EV_STA_CONNECTING`; `esp_wifi_connect`. Dönüş değerleri **denetlenmez** (`c:286,294`).

**Kopma / başarısızlık** `WIFI_EVENT_STA_DISCONNECTED` (`c:460-510`), olay görevinde:
1. `s_is_connected=false`; `SL_EV_STA_LOST` (arg = reason).
2. reason 2 (`AUTH_LEAVE`) veya 8 (`ASSOC_LEAVE`) → biz kopardık, **hiçbir şey yapma** (`c:470-472`).
3. Bağlıyken koptuysa tek satır sys_log WARN (reason, RSSI, TX) (`c:475-479`).
4. `s_retry_count < 5` → sayaç++, gecikme = 1000 ms × sayaç (1,2,3,4,5 s), `s_retry_timer` tek seferlik → `sta_retry_cb` → `esp_wifi_connect` (`c:481-492`). Toplam 5 deneme ≈ 15 s + her denemenin kendi süresi. Olay görevi bloklanmaz (önceki sürümde 5 s'ye kadar bloklanıyordu, `c:485-486`).
5. 5 deneme bitti ve henüz yedek denenmediyse (`!s_trying_backup && backup var`) → "Primary basarisiz. Backup deneniyor" → `do_connect(&s_backup)` **olay görevinde, 200 ms `vTaskDelay` ile** (`c:493-496`).
6. Yedek de bitti → sys_log WARN (bir kez, `s_retry_exhausted_logged`), sayaçlar sıfır, `start_reconnect_task` (`c:497-508`).

**Periyodik yeniden bağlanma** `reconnect_task_fn` (`c:369-389`): 15 s uyku; bağlandıysa kendini siler; retry sürüyorsa (`s_retry_count>0`) atlar; aksi `scan_and_connect`. Sonsuzdur ("tüm denemeler başarısızsa 15 sn'de bir tekrar tara", `c:11`). Görev `stop_reconnect_task` ile dışarıdan `vTaskDelete` edilir (`c:397-404`; çağıranlar: `c:522` GOT_IP, `c:671` connect_sta).

**Bağlandı** `IP_EVENT_STA_GOT_IP` (`c:514-545`): IP metni, `s_is_connected=true`, sayaçlar sıfır, reconnect görevi durdur, `esp_wifi_set_ps(WIFI_PS_NONE)` (`c:527`), `SL_EV_STA_GOT_IP` (arg=RSSI), sys_log INFO, `s_trying_backup=false` (`c:537`), TX kick, SNTP başlat, mDNS hostname'i yeniden ayarla (`c:542-544`).

**Yedekten birincile dönüş:** Otomatik dönüş **yoktur**. Yedek ağa bağlıyken birincil ağ geri gelse bile cihaz yedekte kalır; ancak yedek koparsa yeniden tarama birincili tercih eder. Üstelik GOT_IP'de `s_trying_backup=false` yapıldığı için (`c:537`) yedek ağ kopunca 5 retry'dan sonra kod "Primary basarisiz" deyip **yine yedeği** dener (`c:493-496`); birincil ancak bunun da 5 denemesi bitince, reconnect görevinin taramasıyla denenir.

**AP → STA / STA → AP mod geçişleri** (`ensure_sta_mode` `c:230-264`; `set_ap_enabled` `c:732-767`):
- AP açıkken STA eklenir: `AP → APSTA`. AP kapalıysa `STA`.
- `set_ap_enabled(false)` yalnızca STA bağlıyken kabul edilir (güvenlik: cihaz erişilemez olmasın). Sonra STA koparsa **AP otomatik geri açılmaz** (`c:12`): cihaz router dönene kadar erişilemez. Kalıcıdır (`ap_enabled` NVS), reboot sonrası da AP kapalı.
- `WIFI_MODE_NULL` görülürse (`c:251-261`) mod yeniden kurulup `esp_wifi_start` çağrılır.

#### Statik IP (`apply_static_ip` `c:178-217`)

`ip` boş → `esp_netif_dhcpc_start`. Geçersiz IP → uyarı + DHCP. `mask` boş/geçersiz → 255.255.255.0; `gw` boş → `(ip & mask) | .1`; DNS: verildiyse `MAIN=dns, BACKUP=gw`, verilmediyse `MAIN=gw, BACKUP=8.8.8.8` (`c:201-212`). Birincil ve yedek ağın kendi statik ayarları vardır (`wifi_cred_t.st`).

#### mDNS (`start_mdns` `c:408-424`)

Hostname = cihaz adının küçük harfi (RFC 6762) (`c:417-419`); instance = cihaz adı; `_http._tcp` 80 servisi. mDNS, WiFi başlamadan önce `init` içinde kurulur; STA IP alınca hostname yeniden set edilir (`c:542-544`). Etkin ayar: yalnız duyuru (`CONFIG_MDNS_ENABLE_BROWSE=n`, CLI yok, tek örnek — `sdkconfig.defaults:93-95`), IPv6 kapalı olduğundan yalnız A kaydı. Çözümlenen sürüm: `espressif/mdns 1.14.0` (`esp_bin/HW/dependencies.lock`).

#### SNTP (`start_sntp` `c:946-955`)

İlk GOT_IP'de bir kez `esp_netif_sntp_init("pool.ntp.org")`; `sync_cb` → `sys_log_time_synced()` (açılış epoch'u RTC'ye). `s_sntp_started` kilidi: bir daha init edilmez. Saat dilimi ayarı yok (UTC). Saat yalnızca sys_log zaman damgası ve `api_client` `{time}` için kullanılır; cihazın geri sayımı saate bağlı değildir.

#### sys_log entegrasyonu

Olay işaretleri: `STA_CONNECTING c:293`, `AP_CLIENT_JOIN c:437`, `AP_CLIENT_LEAVE c:455`, `STA_LOST c:466`, `STA_GOT_IP c:531`, `TX_POWER c:892`. Flash'a satır: `c:439,477,502,533,751,756,893`. WiFi bağlamı (mod, istemci, RSSI, TX) her değişimde `update_log_ctx` (`c:926-936`) ile RTC'ye.

### 1.4 Mesaj içerikleri

Bu modül dış mesaj üretmez. Kullanıcıya görünen metinler sys_log satırlarıdır (Türkçe, diyakritiksiz): "WiFi koptu: '%s' (reason %d, rssi %d, tx %d dBm)" (`c:477`), "WiFi baglanamadi: '%s' (son reason %d), %d sn'de bir taranacak" (`c:502`), "WiFi baglandi: '%s' IP %s (%s, rssi %d, tx %d dBm)" (`c:533`), "AP modu acildi/kapatildi…" (`c:751,756`), "TX gucu ayari: …" (`c:893`), "AP: istemci baglandi (AID %d, toplam %d, tx %d dBm)" (`c:439`).

### 1.5 Kalıcı veri — NVS namespace `wifi_cfg`

| Anahtar | Tip | Üst sınır | Varsayılan | Yazan | Okuyan |
|---|---|---|---|---|---|
| `p_ssid` | str | 32+NUL | boş | `connect_sta c:664`, `clear_primary c:777` | `load_credentials c:144` |
| `p_pass` | str | 64+NUL | boş | aynı | aynı |
| `p_ip`, `p_gw`, `p_mask`, `p_dns` | str | 15+NUL | boş (DHCP) | aynı | aynı |
| `b_ssid`, `b_pass`, `b_ip`, `b_gw`, `b_mask`, `b_dns` | str | yukarıdaki gibi | boş | `save_backup c:721`, `clear_backup c:791` | `c:145` |
| `ap_enabled` | u8 | 0/1 | 1 | `set_ap_enabled c:762` | `init c:559` |
| `tx_max` | u8 | 8..20 (dışı yok sayılır) | 17 | `set_tx_power c:898` | `init c:562` |
| `tx_adapt` | u8 | 0/1 | 1 | `set_tx_power c:899` | `init c:566` |
| (eski) `ssid`, `password`, `static_ip`, `backup_ssid`, `backup_pass`, `backup_ip` | str | — | — | silinir | bir kez `p_*/b_*`'ye taşınır (`c:147-166`) |

Kurallar: boş değer anahtarı **siler** (`set_or_erase c:120-124`). Yükleme `NVS_READWRITE` açar ve her açılışta `nvs_commit` yapar (`c:142,167`). Parolalar **düz metin**; `CONFIG_NVS_ENCRYPTION=n` (`sdkconfig.defaults:33`). Ayrıca `CONFIG_ESP_WIFI_NVS_ENABLED=y` (etkin) olduğundan WiFi sürücüsü son STA SSID/parolasını kendi `nvs.net80211` alanında **ikinci bir kopya** olarak tutar; `clear_primary` bunu silmez, yalnızca fabrika sıfırlama (`nvs_flash_erase`, `main.c:108`) siler.

### 1.6 Güç yönetimi

Gerekçe (dosya başı `c:14-22`, commit `acde5dc`, hafıza notu): PCB rev 1 sürekli donuyor/resetleniyordu, rev 2 ~72 saatte bir, rev 3 telefon AP'ye bağlandıktan ~15 s sonra reset attı. TX anındaki pik akım karşı cihazdan bağımsız, TX gücüne (dBm) bağlı; zayıf beslemede pik → brownout. sys_log'un "cause=brownout + son olay AP_CLIENT_JOIN/GUI_SERVE" kaydı bu tezi doğrulamak için tutuluyor.

Mekanizma:
- **Yumuşak başlangıç:** radyo 8 dBm ile açılır (`c:611`), 2,5 s sonra (kondansatörler dolunca) hedefe çıkılır (`c:613`, `tx_power_kick`).
- **Tavan:** varsayılan 17 dBm (20 yerine), kullanıcı 8..20 (`h:10-13`, GUI slider).
- **Uyarlanabilir** (`tx_power_evaluate c:831-875`, her 10 s + olay sonrası 2,5 s): bağlı linklerin en zayıf RSSI'si (AP istemcileri `esp_wifi_ap_get_sta_list` + router linki `esp_wifi_sta_get_ap_info`); RSSI 0 = henüz ölçülmedi, atlanır. `rssi_to_dbm` (`c:806-813`): ≥-55 → 8, ≥-62 → 11, ≥-70 → 14, ≥-76 → 17, aksi 20; sonuç tavanla sınırlanır. Hiç link yoksa tavan (keşif/beacon menzili). **Yükselme anında, düşürme 2 ardışık örnekte** (histerezis, `c:864-874`); olay tetikli değerlendirmede histerezis tek adıma indirilir (`c:880`).
- **AP istemcisi bağlandığı an** (telefon yakındadır, GUI yüklenecek): `tx_now > 14` ise hemen 14 dBm (`c:445-447`), ardından RSSI'ye göre düzeltme.
- **Uygulama:** `esp_wifi_set_max_tx_power(dbm*4)` (0,25 dBm birimi, `c:821`); cihaz geneli (AP ve STA ortak).
- **Modem uyku:** STA IP alınca `WIFI_PS_NONE` (`c:527`); yorum: modem-sleep ortalama akımı düşürür, piki değil; zayıf sinyalde kararlılık için kapalı. Bağlanana kadar IDF varsayılanı (`WIFI_PS_MIN_MODEM`) geçerlidir; bir daha açılmaz.

Nicemleme notu (IDF `esp_wifi.h:1117-1123`): sürücü 11 kademeli bir tablo uygular; `8→32→28 (7 dBm)`, `17→68→66 (16,5 dBm)`, `9,10→34 (8,5)`, `12→44 (11)`, `16→60 (15)`, `19→72 (18)`. Yani GUI'deki 13 tam sayı değeri fiilen 11 seviyeye düşer ve "8 dBm" gerçekte 7 dBm'dir. İşlevsel bir sorun değil, belgeleme/ beklenti meselesi.

### 1.7 Bağımlılıklar

`CMakeLists.txt`: `esp_wifi esp_netif esp_event nvs_flash mdns freertos sys_log esp_timer lwip`. Yönetilen bileşen: `espressif/mdns` (`main/idf_component.yml`, sürüm `*` → 1.14.0). `esp_netif_sntp` (`esp_netif` içinde). Proje modülü: `sys_log`. Dışarıdan radyoya dokunan: `system_info.c:63-66`.

### 1.8 Gördüğüm sorunlar

1. **AP kapalıyken STA koparsa cihaz erişilemez** — `c:12` bilinçli tasarım, ama bir dead man's switch için kritik: router değişirse/ölürse cihaz GUI'siz kalır; tek çıkış fabrika sıfırlama (buton 5x). Zaman aşımlı otomatik AP geri açma yok.
2. **Bağlantı hatası nedeni kullanıcıya ulaşmaz:** `connect_sta` daima `ESP_OK` döner (`c:676`); `/api/wifi/config` yalnız `connected` verir. Yanlış parola (reason 15/202) ile menzil dışı (201) GUI'de aynı görünür.
3. **Reconnect görevinin dışarıdan `vTaskDelete` edilmesi** (`c:397-404`): görev `scan_and_connect` içindeyken (malloc'lu liste `c:328`, engelleyici tarama) öldürülürse heap sızıntısı ve sürücü tarama durumu belirsiz. `connect_sta` httpd'den her an çağırabilir (`c:671`).
4. **Tarama sırasında `connect_sta`:** tarama bitmeden `esp_wifi_disconnect/set_config/connect` (`c:281-294`) çağrılır; dönüşler denetlenmediği için kullanıcı isteği sessizce düşebilir **(koddan çıkarım)**.
5. **`esp_wifi_scan_get_ap_records(&n, NULL)`** (`c:339`): malloc başarısızsa listeyi boşaltmak için yanlış çağrı; IDF'in belgelediği yol `esp_wifi_clear_ap_list()` (`esp_wifi.h:617,644`). Tarama sonuçları bir sonraki taramaya kadar sürücüde kalır.
6. **Olay görevinde engelleme:** yedeğe geçişte `do_connect` olay görevinde 200 ms uyur (`c:496→282`); her STA/AP olayında `sys_log_write` harici SPI flash'a dosya ekler (mutex + fopen/fwrite, `sys_log.c:624-628`). Olay görevi yığını bu yüzden 4 KB'a çıkarılmış (`sdkconfig.defaults:46-47`). AP/IP/mDNS olayları flash yazımı kadar gecikir.
7. **`s_ip_addr` kopmada sıfırlanmaz:** STA_DISCONNECTED (`c:460-510`) IP metnine dokunmaz → GUI `connected:false` ile birlikte **eski STA IP'sini** gösterir (`web_server.c:716,841`).
8. **`IP_EVENT_STA_LOST_IP` işlenmez** (`c:585-586`): DHCP kirası düşerse `s_is_connected` true kalır.
9. **APSTA'da 15 s'de bir engelleyici tarama:** router menzil dışındayken (tezgâh durumu) AP istemcileri her 15 s'de ≈1,6–6,5 s kanal dışı kalır; GUI'de takılma olarak görünebilir **(koddan çıkarım, ölçülmedi)**.
10. **Her iki ağ görünürken RSSI karşılaştırması yok** (`c:348-355`); birincil -90 dBm, yedek -50 dBm olsa da birincile bağlanır.
11. **Yedekten birincile dönüş yok** ve kopma sonrası log mesajı yanıltıcı ("Primary basarisiz" derken yedeği yeniden deniyor, `c:493-496`, bkz. 1.3).
12. **`ESP_ERROR_CHECK` ile panik** (`c:575-586,605-607`): WiFi init başarısızsa cihaz boot döngüsüne girer; sys_log'a "wifi" satırı düşmeden önce ölür (`main.c:488-491` hiç ulaşılmaz).
13. **AP parolasız** (`c:224`): yakındaki herkes `192.168.4.1`'e ulaşır; koruma yalnızca isteğe bağlı tarayıcı parolası (web_server). Ayrı bir belge konusu ama ağ katmanında görünür.
14. **Paylaşılan durum kilitsiz:** `s_primary/s_backup/s_sta_ssid` httpd'de yazılır, olay/reconnect görevinde okunur; `strncpy` ile kısmi yazım anında okuma mümkün (düşük olasılık).
15. **Sabitler:** `8.8.8.8` (`c:210`), `pool.ntp.org` (`c:949`), `"192.168.4.1"` iki yerde metin (`c:46,615`; `web_server.c:704`), cihaz hostname'i DHCP'ye gönderilmez: `esp_netif_set_hostname` hiçbir bileşende çağrılmıyor (grep ile doğrulandı) → router'ın istemci listesinde IDF varsayılan adı görünür **(görünen ad koddan çıkarım)**.
16. **Yükleme her açılışta `NVS_READWRITE` + `nvs_commit`** (`c:142,167`): gereksiz flash yazımı değil (commit boşsa yazmaz) ama okunurluk/niyet açısından karışık.

### 1.9 Korunması gereken davranışlar (test edilebilir)

- [ ] Açılışta AP SSID = cihaz adı (`LS-` + 10 karakter), parolasız, IP 192.168.4.1, en fazla 4 istemci; GUI AP üzerinden `auto_connect` başlamadan erişilebilir.
- [ ] Kayıtlı ağ yoksa cihaz AP'de kalır; log "Kayitli WiFi yok".
- [ ] Kayıtlı ağ varsa açılışta tarama → birincil görünürse birincil, yalnız yedek görünürse yedek, hiçbiri görünmüyorsa gizli ağ varsayımıyla kör deneme.
- [ ] STA bağlantısı AP'yi **kapatmaz** (APSTA); AP kanalı STA kanalına uyar.
- [ ] Kopmada 5 deneme (1,2,3,4,5 s gecikmeli), sonra yedek, sonra 15 s'de bir sonsuz tarama; olay görevi bloklanmaz.
- [ ] Kendi başlattığımız kopmalar (reason 2/8) yeniden deneme tetiklemez.
- [ ] `connect_sta` parola boş ve SSID aynıysa kayıtlı parolayı korur; aynı SSID+parola+statik ayara zaten bağlıysa hiçbir şey yapmaz; yeni kimlik bağlantı sonucundan bağımsız NVS'ye yazılır.
- [ ] Statik IP: `ip` boş → DHCP; mask/gw/dns türetme kuralları (255.255.255.0, x.x.x.1, gw+8.8.8.8).
- [ ] AP, STA bağlı değilken kapatılamaz (`ESP_ERR_INVALID_STATE` → GUI "WiFi bagli degil, AP kapatilamaz"); kapatma kalıcıdır; otomatik geri açılmaz (bu davranışın korunup korunmayacağı açık soru).
- [ ] mDNS `ls-xxxxxxxxxx.local` (küçük harf), `_http._tcp:80`.
- [ ] SNTP yalnız STA bağlanınca; `sys_log_time_synced` çağrılır.
- [ ] TX: açılış 8 dBm → 2,5 s sonra hedefe; tavan NVS'de (varsayılan 17); uyarlanabilir modda AP istemcisi bağlanınca ≤14 dBm; yükselme hemen, düşürme 2 örnek histerezisli; `POST /api/wifi/txpower` hemen uygular ve NVS'ye yazar; 8..20 dışı `ESP_ERR_INVALID_ARG`.
- [ ] STA IP alınınca modem uyku kapalı.
- [ ] Eski NVS anahtarları (`ssid/password/static_ip`, `backup_*`) bir kez `p_*/b_*`'ye taşınır (2611'e geçişte yalnız `p_*/b_*` taşınacaksa bu kural da taşınmalı; eski anahtarlı cihaz sahada varsa).
- [ ] `/api/wifi/config` alanları: `connected, ip, sta_ssid, static{}, backup_ssid, backup_static{}, has_backup, ap_enabled, ap_clients, rssi, tx_max, tx_now, tx_adaptive` (`web_server.c:824-852`).
- [ ] sys_log'a yazılan WiFi olayları: bağlandı (rssi, tx), koptu (reason, rssi, tx), bağlanamadı (bir kez), AP istemci bağlandı, AP açıldı/kapandı, TX ayarı.

### 1.10 Açık sorular

- AP kapalıyken STA kaybı: belirli bir süre (örn. 10 dk) bağlanamazsa AP otomatik açılsın mı? Mevcut tasarım "asla" diyor; güvenlik (yabancı erişimi) ile erişilebilirlik çelişiyor.
- README (`ALTE/README.md:65,338`) AP adını "SmartKraft-LebensSpur" diye yazıyor; kod cihaz adını kullanıyor. Hangisi doğru?
- İki ağ da görünürken RSSI'ye göre seçim istenir mi, yoksa "birincil her zaman öncelikli" kural mı?
- Yedek ağdayken birincil geri gelince dönüş istenir mi?
- WiFi hata nedeninin (yanlış parola / menzil dışı) GUI'ye taşınması istenir mi?
- Saat dilimi: loglarda UTC yeterli mi?

---

## 2. `smtp_client`

Dosyalar: `smtp_client.c` (778 satır), `include/smtp_client.h` (37 satır).

### 2.1 Modül amacı

Kullanıcının kendi SMTP hesabından (Gmail uygulama parolası, Outlook, Proton Bridge, özel sunucu) e-posta gönderir. İki yol: `smtp_client_send` (düz metin) ve `smtp_client_send_with_attachments` (MIME multipart, harici flash'taki dosyalar base64). TLS elle mbedTLS ile lwIP soketi üzerine kurulur (esp-tls kullanılmaz); STARTTLS (587 vb.) ve doğrudan TLS (465) desteklenir. Yalnızca `AUTH LOGIN`.

### 2.2 Dış arayüz

| Fonksiyon | Ne yapar | Dönüş / hata | Engelleyici | Çağıran |
|---|---|---|---|---|
| `smtp_client_init(void)` `c:80-91` | `ls_smtp`'den 4 anahtarı yükler; **dördü de yoksa** yapılandırılmamış sayar (`c:43-56`) | her zaman `ESP_OK` | Hayır | `main.c:480` |
| `smtp_client_save_config(const smtp_config_t*)` `c:93-100` | RAM + NVS; `nvs_set_*` dönüşleri denetlenmez | `nvs_open` hatası ya da `ESP_OK` | Hayır | `web_server.c:1096` |
| `smtp_client_get_config(smtp_config_t*)` `c:102-107` | RAM kopyası (api_key dahil) | `ESP_OK` | Hayır | `web_server.c:1044,1087` |
| `smtp_client_has_config(void)` `c:109-112` | `s_configured` | bool | Hayır | main, web_server |
| `smtp_client_send(to, subject, body)` `c:280-528` | Tek alıcıya düz metin mail; TCP → (STARTTLS) → TLS → AUTH LOGIN → MAIL/RCPT/DATA → QUIT | `ESP_ERR_INVALID_STATE` (ayar yok); `ESP_ERR_INVALID_ARG` (AUTH 235 değil); `ESP_ERR_NO_MEM`; `ESP_FAIL` (diğer her şey); **sunucunun maili reddetmesi `ESP_OK` döner** (bkz. 2.8) | **Evet**, sınırsız: her komut 15 s okuma zaman aşımı, `connect()` için uygulama zaman aşımı yok, TLS el sıkışma döngüsü sonsuz olabilir | `main.c:154,313`; `web_server.c:1114` (test, httpd) |
| `smtp_client_send_with_attachments(to, subject, body, file_paths[], n)` `c:559-757` | `n<=0` → `smtp_client_send`; aksi aynı akış + multipart/mixed, dosyalar 570 B parçalarla base64 | aynı; açılamayan dosya **atlanır** (`c:683-686`) | Evet, dosya boyutuyla orantılı | `main.c:309` |
| `smtp_client_send_test(void)` `c:759-778` | Kendi adresine Türkçe (diyakritiksiz) test maili | `smtp_client_send` sonucu | Evet | `web_server.c:1114` |

`smtp_config_t` (`h:7-12`): `server[64] port(u16) user[64] api_key[64]`.

### 2.3 Davranış

#### Protokol akışı (`smtp_client_send`)

1. mbedTLS bağlamları **yığında** (`c:289-297`), RNG tohumla (`c:306`), istemci varsayılanları (`c:313`), **`MBEDTLS_SSL_VERIFY_OPTIONAL`** (`c:322`), ESP sertifika paketi (`c:326`), SNI hostname = sunucu (`c:328`), 1 KB heap tampon (`c:341`).
2. `smtp_tcp_connect` (`c:227-276`): `getaddrinfo` **yalnız IPv4**, ilk adres; `SO_RCVTIMEO/SO_SNDTIMEO` 15 s; `connect()` engelleyici, zaman aşımı yok (lwIP SYN yeniden denemeleri `CONFIG_LWIP_TCP_SYNMAXRTX=12`'ye bağlı; süre ölçülmeli).
3. Port ≠ 465 → **STARTTLS** (`c:359-404`): greeting oku → `EHLO lebensspur` (düz) → `STARTTLS` → yanıt `220` ile başlamalı (`c:382`, pre-TLS **tek** kod denetimi) → el sıkışma → `EHLO lebensspur` (TLS). Port = 465 → **doğrudan TLS** (`c:406-434`): el sıkışma → greeting → EHLO.
4. **AUTH LOGIN** (`c:436-471`): `AUTH LOGIN` → base64(user) → base64(api_key) → yanıt `235` değilse `ESP_ERR_INVALID_ARG`. PLAIN, CRAM-MD5, XOAUTH2 **yok**; EHLO yanıtı ayrıştırılmaz (sunucunun desteklediği AUTH yöntemleri bakılmaz).
5. `MAIL FROM:<user>` (`c:475`), `RCPT TO:<to>` (`c:483`), `DATA` (`c:491`) — **yanıt kodları denetlenmez**, yalnız okuma başarısı.
6. Başlık + gövde + `\r\n.\r\n` tek `snprintf` (`c:497-505`), yanıt `250` değilse yalnız **WARN log** (`c:510-512`), `err` değişmez.
7. `QUIT` (`c:515`, yanıt yok sayılır). "Mail basariyla gonderildi" logu (`c:517`) her durumda.
8. cleanup (`c:519-527`): close_notify, free, soket kapat.

Her sunucu yanıtı **tek `recv`** ile okunur (`c:118`, `c:177`); çok satırlı EHLO yanıtı birden çok TCP parçasında gelirse sonraki komutun yanıtı olarak yanlış okunur (bkz. 2.8).

Sertifika doğrulama düzeyi: **fiilen yok**. `VERIFY_OPTIONAL` el sıkışmayı sertifika geçersiz olsa da tamamlar; `mbedtls_ssl_get_verify_result` hiç çağrılmaz. `sdkconfig.defaults:68` bunu bilinçli ("teslimat öncelikli") diye not eder.

Yeniden deneme / geri çekilme: **yok**, hiçbir katmanda (modül, `main.c`). Her çağrı tek atış; hata sys_log'a yazılır, bitti.

Eşzamanlılık: kilit yok. `alarm_mail`, `trig_mail` ve httpd testi aynı anda koşabilir; her biri kendi mbedTLS oturumunu kurar (heap: in 16 KB + out 4 KB + el sıkışma, etkin `CONFIG_MBEDTLS_SSL_IN/OUT_CONTENT_LEN`).

#### Ekli gönderim farkları (`c:559-757`)

- Bağlantı+AUTH kodu **satır satır kopyalanmış** (`c:575-648`), hata logları **silinmiş** (yalnız `err=ESP_FAIL; goto`): ekli maillerin başarısızlık nedeni loglara düşmez.
- Başlıklar (`c:662-673`): `From: LebensSpur <user>`, `To`, `Subject`, `MIME-Version: 1.0`, `multipart/mixed; boundary="----LS-MIME-BOUNDARY-2026"` (`c:659`), ilk parça `text/plain; charset="UTF-8"`.
- Her dosya (`c:677-734`): `Content-Type` uzantıdan (`get_mime_type c:532-548`: pdf, txt, jpg/jpeg, png, gif, mp3, wav, mp4, doc, docx; aksi `application/octet-stream`), `Content-Transfer-Encoding: base64`, `Content-Disposition: attachment; filename="<basename>"` (RFC 2231/2047 kodlaması yok; `"` içeren ad başlığı bozar). 570 B ham → 760 karakter base64 + tek CRLF: satır uzunluğu 760 (RFC 2045'in 76 sınırını aşar, SMTP'nin 998 sınırının altında; yorum `c:709,722` "her 76 karakterde satır sonu" der ama kod yapmaz). `fread` hatası sessizce `break` (`c:718-721`) → kesik ek + "başarılı".
- Alıcı başına ayrı oturum: `main.c:302-328` her alıcı için dosyaları **yeniden okur ve yeniden gönderir** (BCC/çoklu RCPT yok).

### 2.4 Mesaj içerikleri

| Mail | Alıcı | Konu | Gövde | Ek | Dil |
|---|---|---|---|---|---|
| Test (`c:759-778`) | kendi adresi (`user`) | "LebensSpur - SMTP Test Basarili" (sabit) | sabit Türkçe metin (diyakritiksiz), sunucu/port/gönderen | yok | TR sabit |
| Erken uyarı (`main.c:117-165`) | `ls_ew_mail.email` (tek adres, 63 karakter) | `ls_ew_mail.subject` (63) | `ls_ew_mail.body` (255) | yok | kullanıcı metni; GUI placeholder 6 dilde (`GUI/lang/*.json: early_mail.body_ph`), cihaza dil bilgisi gitmez |
| Tetikleme (`main.c:230-339`) | `ls_tg_mail.g<i>_rcpt` virgülle ayrılmış (255 karakter toplam), grup başına döngü | `g<i>_subj` (63) | `g<i>_body` (255) | `/ext/mail/g<i>/*` en fazla 5 dosya × 2 MB | kullanıcı metni |

Başlık şablonu (`c:497-505`): `From: LebensSpur <user>` / `To: <to>` / `Subject: <subject>` / `Content-Type: text/plain; charset=UTF-8` / boş satır / gövde / `.`. **Yok:** `Date`, `Message-ID`, `MIME-Version` (düz mailde), `Content-Transfer-Encoding`; konu RFC 2047 ile kodlanmaz (Türkçe/Almanca diyakritik ham 8-bit gider); gövdede satır başı `.` için dot-stuffing yok (RFC 5321 §4.5.2 — gövde bu satırda kesilir). Yer tutucu yok (`{device}` vb. SMTP'de **desteklenmez**; yalnız api_client'ta var). Erken uyarı ile tetiklenme mailleri arasında yapısal fark yok; fark yalnız kaynağın NVS alanı ve ek dosyalarıdır.

CR/LF enjeksiyonu: `web_server.c:1081-1082,1508-1509,1612-1613` konu/alıcı/sunucu alanlarından CR/LF temizler; gövde temizlenmez (gövde için gereksiz).

### 2.5 Kalıcı veri — NVS namespace `ls_smtp`

| Anahtar | Tip | Sınır | Varsayılan | Yazan | Okuyan |
|---|---|---|---|---|---|
| `server` | str | 63 | — | `save_to_nvs c:69` | `load_from_nvs c:44` |
| `port` | u16 | — | GUI boş/0 → 465 (`web_server.c:1077`) | `c:70` | `c:47` |
| `user` | str | 63 | — | `c:71` | `c:51` |
| `api_key` | str | 63 | — | `c:72` | `c:55` |

İlişkili (main/web_server'ın sahibi olduğu, SMTP'nin tükettiği):
- `ls_ew_mail`: `email`(63) `subject`(63) `body`(255) — yazan `web_server.c:1517-1519`, okuyan `main.c:138-144`.
- `ls_tg_mail`: `count`(u8, ≤10), `g<i>_name`(63) `g<i>_subj`(63) `g<i>_body`(255) `g<i>_rcpt`(255), i=0..9 — yazan `web_server.c:1632-1646`, silme/kaydırma `1686-1732`, okuyan `main.c:255-271`.
- `ls_actions`: `early_mail`, `trig_mail` (u8, varsayılan 0) — yazan `web_server.c:1820-1825`, okuyan `main.c:122,235`.
- Dosya sistemi: `/ext/mail/g<i>/<dosya>` (harici flash LittleFS, yazan `web_server.c:2099-2108`, grup silinince kaydırılır `1737-1758`).

Sırlar: `api_key` düz metin, NVS şifresiz. GUI'ye hiç dönmez (`web_server.c:1051-1054` yalnız server/port/user); boş gönderilirse mevcut korunur (`web_server.c:1085-1090`). **Ancak** AUTH sırasında base64 parola satırının ilk 64 karakteri `ESP_LOGI` ile konsola basılır (`c:462 → c:209 → c:191`); base64 geri çevrilebilir.

NVS bölümü 24 KB (`partitions.csv:3`); tüm namespace'lerin üst sınırı (ls_tg_mail ≈ 6,4 KB + ls_api ≈ 1,7 KB + diğerleri) sığar, ama dolu 10 grup + uzun gövdelerle sayfa döngüsü için pay azalır — ölçülmeli.

### 2.6 Güç yönetimi

Modülün kendi güç mantığı yok. sys_log `SL_EV_MAIL_SEND` işareti (`main.c:153,307`) "yoğun CPU + TX" dönemini brownout analizi için işaretler. Mail görevleri sırasında TX gücü `wifi_manager`'ın uyarlanabilir değerindedir; mail gönderimi için güç yükseltme/düşürme yoktur.

### 2.7 Bağımlılıklar

`CMakeLists.txt`: `mbedtls nvs_flash log lwip`. Başlıklar: `esp_crt_bundle.h` (mbedtls bileşeni), `sys/socket.h`, `netdb.h`. esp-tls **kullanılmaz**; Telegram/webhook ile aynı sertifika paketi ama farklı TLS yığını (bkz. 7).

### 2.8 Gördüğüm sorunlar

1. **Sunucu reddi "başarı" olarak dönüyor** (`c:510-512,741-743`): DATA sonrası `250` gelmezse yalnız WARN; fonksiyon `ESP_OK` döner → `main.c:315-319` "Tetikleme maili gonderildi" yazar, GUI testi "Test maili gonderildi" der. `RCPT TO` için `550` (alıcı yok) da yakalanmaz (`c:484`). Dead man's switch için en ağır bulgu: teslim edilmemiş mail başarılı sanılır.
2. **TLS el sıkışma / yazma döngüsü sonsuz olabilir** (`c:389-396,411-418,626-635,196-200`): soket 15 s zaman aşımında `recv` `EWOULDBLOCK` → `ssl_recv_cb` `WANT_READ` → döngü devam. Karşı taraf sessiz ama TCP açık kaldığında (WiFi kopması, yarı açık bağlantı, NAT zaman aşımı) mail görevi süresiz asılı kalır; `trig_mail` kalan alıcılara hiç geçemez **(koddan çıkarım)**. TWDT yalnız idle'ı izlediği için reset de olmaz.
3. **`connect()` zaman aşımı yok** (`c:265`): kara deliğe giden SYN'ler lwIP tablosuna göre dakikalar sürebilir; `SO_SNDTIMEO` lwIP'te `connect`'e uygulanmaz **(koddan çıkarım, ölçülmeli)**.
4. **Sertifika doğrulaması fiilen kapalı** (`c:322,601`): MITM ile kimlik bilgileri ele geçirilebilir. Bilinçli tercih ama en azından doğrulama sonucu loglanmalı; 2611'de esp-tls+bundle ile `VERIFY_REQUIRED` + kullanıcıya "doğrulamayı gevşet" seçeneği düşünülmeli.
5. **Parola konsol loguna düşüyor** (`c:191` üzerinden `c:462,647`): base64, ilk 64 karakter.
6. **Tek `recv` ile yanıt okuma** (`c:118,177`): çok satırlı/parçalı yanıtlarda komut-yanıt kayması; `STARTTLS` yanıt denetimi `220` yanlış pozitif/negatif olabilir.
7. **Kod kopyası:** `send_with_attachments` bağlantı+AUTH bloğunu tekrar eder (`c:575-648` ↔ `c:289-471`), hata mesajları olmadan.
8. **RFC uyumu:** `EHLO lebensspur` FQDN değil (`c:371,401,430`); `Date`/`Message-ID` yok; 8-bit konu/gövde `8BITMIME` pazarlığı yapılmadan gider; dot-stuffing yok; base64 satırları 760 karakter. Çoğu büyük sağlayıcı tolere eder, spam puanlaması etkilenebilir **(koddan çıkarım)**.
9. **Yığın:** httpd görevinde (8 KB, `web_server.c:2613`) tam mbedTLS el sıkışma + yığındaki bağlamlar (`c:289-292`, `errbuf[128]`, `b64[128]`); ekli yolda ek 1370 B (`c:710-711`). Görev yığınları 10 KB (`main.c:218,358`); en kötü durum ölçülmemiş.
10. **Testler httpd'yi kilitler:** SMTP testi httpd görevinde koşar (`web_server.c:1114`); sürerken (en kötü dakikalar) **hiçbir** web isteği yanıtlanmaz (tek httpd görevi, `max_open_sockets=7`).
11. **Toplu gönderim süresi sınırsız:** 10 grup × ~8 alıcı × (5 ek × 2 MB yeniden gönderim) için üst sınır/öncelik yok; önce küçük mailler, sonra ekli olanlar gibi bir sıralama yok.
12. **Boş konu**: GUI placeholder gönderilmez; kullanıcı boş bırakırsa `Subject: ` boş gider.
13. **Alıcı listesi `rcpt[256]`**: 10 alıcıdan sonrası sessizce kesilir (`web_server.c:1611`).
14. **`ESP_LOGI` ile her gelen/giden satır** (`c:121,130,180,191`): konsolda alıcı adresleri ve gövde görünür; üretimde gürültü ve gizlilik sorunu.

### 2.9 Korunması gereken davranışlar

- [ ] Port 465 → doğrudan TLS; diğer tüm portlar → STARTTLS (587 başta). Port boş/0 → 465.
- [ ] Kimlik doğrulama `AUTH LOGIN` (Gmail uygulama parolası, Outlook, Proton Bridge ile çalıştığı README'de belirtiliyor; yeni mimaride en az LOGIN + PLAIN).
- [ ] ESP sertifika paketi (CMN) ile sunucu sertifikası; doğrulama düzeyi karar gerektirir (bkz. 2.10).
- [ ] Gönderen görünen adı "LebensSpur", zarf gönderici = kullanıcı adresi.
- [ ] Erken uyarı: tek alıcı, `ls_actions.early_mail` kapalıysa gönderilmez, SMTP ayarsızsa sys_log WARN.
- [ ] Tetikleme: grup sırasıyla, her gruptaki her alıcıya ayrı mail; grup dizinindeki dosyalar (≤5) ek olarak; `rcpt` boş grup atlanır; sonunda "N başarılı, M başarısız" sys_log özeti (`main.c:336`).
- [ ] `/api/smtp/get` parolayı döndürmez; `/api/smtp/save` boş parola → mevcut korunur; sunucu/e-posta zorunlu.
- [ ] `/api/smtp/test` auth hatasında "Auth hatasi - email/sifre yanlis", diğer hatalarda "Baglanti hatasi - sunucu/port kontrol edin (…)" (`web_server.c:1118-1121`).
- [ ] Test maili kendi adresine.
- [ ] Konu/alıcı/sunucu alanlarında CR/LF temizliği.

### 2.10 Açık sorular

- Doğrulama: `VERIFY_REQUIRED` varsayılan + "self-signed sunucuya izin ver" anahtarı mı, yoksa mevcut "her zaman gevşek" mi?
- Teslim başarısızlığında yeniden deneme istenir mi (örn. 5 dk / 30 dk / 2 s aralıklarla 3 kez; WiFi gelene kadar kuyruk)? Timer dolduğunda WiFi yoksa mailler **tamamen kaybolur** (bkz. 7).
- Tek oturumda çoklu `RCPT TO` (ekleri bir kez göndermek) kabul edilebilir mi? (Alıcılar birbirini görür; BCC ile çözülebilir.)
- 256 karakterlik gövde sınırı "hayati bilgi/parola" senaryosu için yeterli mi? Belgeler ek olarak gidiyor; metin sınırı tasarım kararı mı?
- OAuth2 (Gmail XOAUTH2) gereksinimi var mı? Google uygulama parolalarını 2FA'ya bağlıyor; README bunu varsayıyor.

---

## 3. `telegram_client`

Dosyalar: `telegram_client.c` (147 satır), `include/telegram_client.h` (26 satır).

### 3.1 Modül amacı

Telegram Bot API `sendMessage` ile tek bir sohbete düz metin mesaj. Yalnızca **erken uyarı** ve GUI testi için kullanılır; tetiklenmede Telegram **gönderilmez** (`main.c:342-364`'te Telegram çağrısı yok; `ls_actions` içinde `trig_tg` anahtarı yok, `web_server.c:1816-1819`).

### 3.2 Dış arayüz

| Fonksiyon | Ne yapar | Dönüş | Engelleyici | Çağıran |
|---|---|---|---|---|
| `telegram_client_init(void)` `c:24-43` | `ls_telegram` → token, chat_id, message (hatalar yok sayılır); `s_initialized=true` | `ESP_OK` | Hayır | `main.c:481` |
| `telegram_client_save_config(cfg)` `c:45-70` | 3 anahtar + commit; her dönüş denetlenir | `ESP_ERR_INVALID_ARG`, `nvs_open` hatası, `ESP_FAIL` (herhangi bir set/commit) | Hayır | `web_server.c:1887` |
| `telegram_client_get_config(cfg)` `c:72-78` | RAM kopyası (token dahil); lazy init | `ESP_OK` | Hayır | `main.c:187`, `web_server.c:1842,1878` |
| `telegram_client_has_config(void)` `c:80-84` | token && chat_id dolu | bool | Hayır | main, web_server |
| `telegram_client_send(text)` `c:86-147` | HTTPS POST `api.telegram.org/bot<token>/sendMessage`, JSON `{chat_id,text}` | `ESP_ERR_INVALID_ARG` (metin boş **veya** HTTP 401/403), `ESP_ERR_INVALID_STATE`, `ESP_ERR_NO_MEM`, `ESP_FAIL` (taşıma hatası, `init` hatası, diğer HTTP kodları), `ESP_OK` (200) | Evet: 10 s zaman aşımı (`c:114`); DNS + TLS el sıkışma + istek | `main.c:191` (alarm_tg), `web_server.c:1905` (httpd) |

`telegram_config_t` (`h:7-11`): `token[72] chat_id[32] message[256]`.

### 3.3 Davranış

- URL `url[180]` (`c:95-96`): 35 + ≤71 + 12 = 118 karakter, sığar.
- Gövde: `ls_json_escape` (`ls_util.c:76-103`: `"` `\` `\n` `\r` `\t` kaçışlı, diğer kontrol karakterleri **atılır**, UTF-8 ham geçer) ile `text` → 512 karakter, `chat_id` → 64; `malloc(len+40)` (`c:99-108`).
- `esp_http_client`: POST, `crt_bundle_attach` → esp-tls üzerinden **sertifika doğrulaması zorunlu** (SMTP'nin aksine), `timeout_ms=10000`, `Content-Type: application/json` (`c:110-124`).
- `parse_mode` yok (düz metin); yanıt gövdesi okunmaz → Telegram'ın `"description"` alanı (örn. "chat not found", HTTP 400) kaybolur, log yalnız "HTTP 400".
- Durum eşlemesi (`c:137-146`): 200 → OK; 401/403 → "token yanlış" (`ESP_ERR_INVALID_ARG`); diğer → `ESP_FAIL`.
- Yeniden deneme yok; kilit yok (`s_config` httpd'de `memcpy` ile yazılırken alarm görevi okuyabilir).

### 3.4 Mesaj içerikleri

| Durum | Metin | Kaynak |
|---|---|---|
| Erken uyarı | `ls_telegram.message` doluysa o; boşsa sabit **"LebensSpur: Zamanlayiciniz yakinda sona erecek!"** (Türkçe, diyakritiksiz, dilden bağımsız) | `main.c:189` |
| Test | sabit **"LebensSpur test mesaji basarili!"** | `web_server.c:1905` |
| Tetikleme | **yok** | — |

Yer tutucu desteği yok (`{device}` vb. ham gider). GUI placeholder 6 dilde (`telegram.msg_ph`), cihaz tarafında dil yok.

### 3.5 Kalıcı veri — NVS namespace `ls_telegram`

| Anahtar | Tip | Sınır | Varsayılan | Yazan | Okuyan |
|---|---|---|---|---|---|
| `token` | str | 71 | boş | `c:53` | `c:32` |
| `chat_id` | str | 31 | boş | `c:54` | `c:34` |
| `message` | str | 255 | boş | `c:55` | `c:36` |

İlişkili: `ls_actions.early_tg` (u8, varsayılan 0; `web_server.c:1818`, `main.c:173`). Token düz metin; GUI'ye dönmez (`web_server.c:1851-1853`); boş gönderilirse korunur (`web_server.c:1876-1881`); chat_id zorunlu (`1883-1885`).

### 3.6 Güç yönetimi

Yok. `SL_EV_TG_SEND` işareti (`main.c:190`).

### 3.7 Bağımlılıklar

`CMakeLists.txt`: `ls_util esp_http_client mbedtls nvs_flash log`. Dolaylı: esp-tls, esp_crt_bundle, lwIP DNS.

### 3.8 Gördüğüm sorunlar

1. **Tetiklenmede Telegram yok** — GUI'de Telegram yalnız "erken uyarı" kartında; README "Telegram" demiyor zaten. Bilinçli mi, eksik mi? (bkz. 3.10)
2. `ESP_ERR_INVALID_ARG` iki farklı anlam taşır (boş metin / auth hatası) (`c:88,142`); web_server ikisini "Auth hatasi" gösterir (`web_server.c:1909`).
3. Yanıt gövdesi okunmadığı için hata nedeni kaybolur (`c:126-146`).
4. Test httpd görevinde koşar (`web_server.c:1905`), 10 s'ye kadar GUI kilitlenir.
5. Varsayılan erken uyarı metni ve test metni sabit Türkçe (`main.c:189`, `web_server.c:1905`), 6 dilli GUI ile uyumsuz.
6. `json_escape` 0x20 altı karakterleri sessizce atar (`ls_util.c:97`); mesajda sekme/satır sonu dışındaki kontrol karakterleri kaybolur (önemsiz).
7. Token URL'de taşınır (Telegram API gereği) — `esp_http_client` DEBUG seviyesinde URL'yi loglar; INFO'da güvenli.

### 3.9 Korunması gereken davranışlar

- [ ] `has_config` = token **ve** chat_id dolu.
- [ ] Erken uyarıda: `ls_actions.early_tg` kapalıysa gönderme; ayarsızsa sys_log WARN; şablon boşsa varsayılan metin.
- [ ] HTTPS, sertifika doğrulamalı (bundle), 10 s zaman aşımı.
- [ ] 200 → başarı; 401/403 → "token yanlış" GUI mesajı; diğer → genel hata (`web_server.c:1907-1913`).
- [ ] `/api/telegram/get` token döndürmez; boş token → mevcut korunur; chat_id zorunlu.
- [ ] Düz metin (parse_mode yok): kullanıcı metnindeki `*`, `_` olduğu gibi görünür.

### 3.10 Açık sorular

- Tetiklenmede de Telegram gönderilmeli mi (kayıtlı kişilere değil, kullanıcının kendi sohbetine)?
- Çoklu chat_id (aile grubu + kendisi) istenir mi?
- Erken uyarı metninde `{remaining}` / `{device}` gibi yer tutucular istenir mi?

---

## 4. `api_client` (webhook + MQTT)

Dosyalar: `api_client.c` (291 satır), `include/api_client.h` (42 satır).

### 4.1 Modül amacı

Erken uyarı ("alarm") ve tetiklenme ("triggered") anında dış sistemlere bildirim: bir HTTP(S) webhook (GET/POST/PUT, özel başlıklar, şablonlu gövde) ve bir MQTT yayını (bağlan-yayınla-kapat). Şablon yer tutucuları `{status} {time} {device}` (`h:8-11`). Ev otomasyonu (Home Assistant) hedefli (GUI placeholder `homeassistant/lebensspur/state`).

### 4.2 Dış arayüz

| Fonksiyon | Ne yapar | Dönüş | Engelleyici | Çağıran |
|---|---|---|---|---|
| `api_client_init(void)` `c:31-51` | `ls_api`'den 9 anahtar; `wh_method` boşsa `POST` | `ESP_OK` | Hayır | `main.c:482` |
| `api_client_get_config / save_config` `c:53-82` | RAM kopyası / NVS 9 anahtar + commit | `ESP_ERR_INVALID_ARG`, `nvs_open`/`commit` hatası, `ESP_OK` | Hayır | `web_server.c:445,486,505` |
| `api_client_has_webhook` `c:84` | `wh_url` dolu | bool | Hayır | main, web_server |
| `api_client_has_mqtt` `c:85` | `mq_url` **ve** `mq_topic` dolu | bool | Hayır | aynı |
| `api_client_send_webhook(status)` `c:135-201` | Tek HTTP isteği; 2xx → OK | `ESP_ERR_INVALID_STATE`, `ESP_ERR_NO_MEM`, `ESP_FAIL` (init/2xx dışı), taşıma hatası kodu | Evet: 10 s + 3 yönlendirme | `notify_all`; `web_server.c:524` (httpd test) |
| `api_client_send_mqtt(status)` `c:222-274` | Yeni istemci, bağlan (≤8 s), QoS1 yayın, PUBACK bekle (≤5 s), kapat | `ESP_ERR_INVALID_STATE`, `ESP_ERR_NO_MEM`, `ESP_FAIL`, `ESP_ERR_TIMEOUT`, `ESP_OK` | Evet: ≤ 8 + 5 s + kapanış | `notify_all`; `web_server.c:521` |
| `api_client_notify_all(status)` `c:276-291` | Yapılandırılmış kanallara sırayla; sonuçlar sys_log'a; ikisi de yoksa WARN | void | Evet (toplam) | `main.c:208` (`alarm_api`/`trig_api`) |

`api_config_t` (`h:17-27`): `wh_url[256] wh_method[8] wh_headers[256] wh_body[512] mq_url[128] mq_topic[128] mq_user[64] mq_pass[64] mq_payload[256]`.

### 4.3 Davranış

#### Şablon (`expand c:99-131`)

`{status}` → `"alarm" | "triggered" | "test"`; `{time}` → `time(NULL)` eğer > 1700000000 (SNTP senkron) aksi çalışma süresi saniye (`c:105-108`); `{device}` → `LS-…`. Değerler JSON-kaçışlanmaz (hepsi güvenli karakter kümesi). Bilinmeyen `{x}` olduğu gibi kalır. Çıktı tampona güvenli kırpılır.

#### Webhook (`c:135-201`)

- Yöntem: `GET` / `PUT` / aksi `POST` (`c:140-142`; kayıtta web_server da aynı normalizasyonu yapar `web_server.c:498`).
- Gövde (GET hariç): şablon doluysa `expand`, boşsa varsayılan `{"event":"<status>","device":"<dev>","time":<epoch>}` (`c:151-153`; burada `time(NULL)` **doğrudan**, senkron yoksa küçük sayı — `expand`'daki çalışma süresi yedeği yok).
- `esp_http_client`: `timeout_ms=10000`, `crt_bundle_attach` (https için doğrulama **zorunlu**), `max_redirection_count=3` (`c:157-163`). `http://` kabul edilir → başlıklar (ör. `Authorization`) düz metin gider.
- Başlıklar: `wh_headers` cJSON ile nesne olarak ayrıştırılır; yalnız string değerler; `Content-Type` verilmişse varsayılan `application/json` eklenmez (`c:167-187`). Geçersiz JSON → WARN ve atla.
- URL'de yer tutucu **genişletilmez** → GET webhook'ları durum taşıyamaz.
- `SL_EV_WEBHOOK` (arg 0) (`c:189`); sonuç 2xx → `ESP_OK`, aksi `ESP_FAIL`; yanıt gövdesi atılır.

#### MQTT (`c:205-274`)

- Her çağrıda yeni `esp_mqtt_client` (`c:238-245`): `broker.address.uri`, isteğe bağlı kullanıcı/parola, `network.timeout_ms=8000`, `disable_auto_reconnect=true`. **Client ID verilmez** (esp-mqtt varsayılanı `ESP32_<MAC>`), keepalive varsayılan, LWT yok, temiz oturum (varsayılan).
- Olay grubu bitleri: CONNECTED / PUBLISHED / ERROR|DISCONNECTED (`c:205-220`).
- Akış: `start` → CONNECTED|ERROR bekle 8 s → `publish(topic, payload, len=0→strlen, qos=1, retain=0)` (`c:255`) → PUBLISHED|ERROR bekle 5 s → `stop` + `destroy` (`c:267-268`). Bağlantı ömrü: tek yayın.
- Yük: şablon ya da varsayılan `{"state":"<status>","device":"<dev>"}` (`c:231-232`), 512 B yığın tamponu.
- `SL_EV_WEBHOOK` (arg 1) (`c:249`) — webhook ile aynı olay kodu, arg ile ayrılır.
- **`mqtts://`:** `broker.verification.*` hiç verilmez. esp-mqtt bundle/CA yoksa `esp_mqtt_set_cert_key_data(NULL)` çağrısı NULL'u sessizce kabul eder (`mqtt_client.c:157` → `88-90` `if (!data) return ESP_OK`), esp-tls ise doğrulama seçeneği olmayan yapılandırmayı **reddeder** (`esp_tls_mbedtls.c:987-988`, "No server verification option set") çünkü `CONFIG_ESP_TLS_INSECURE` kapalı (etkin sdkconfig:808). Sonuç: `mqtts://` kaydedilebilir (`web_server.c:502`) ve `sdkconfig.defaults:90` yorumu destekliyor der ama bağlantı MQTT_EVENT_ERROR ile düşer **(koddan çıkarım, cihazda sınanmadı)**.

#### `notify_all` ve çağrı koşulları

`main.c:222-224`: erken uyarıda `ls_actions.early_api=1` ise `alarm_api` görevi (`"alarm"`); `main.c:361-363`: tetiklenmede `trig_api=1` ise `trig_api` görevi (`"triggered"`). GUI testi `"test"` ile doğrudan httpd görevinde.

### 4.4 Mesaj içerikleri

| Kanal | Varsayılan yük | Şablon alanı | Yer tutucular |
|---|---|---|---|
| Webhook POST/PUT | `{"event":"alarm\|triggered\|test","device":"LS-…","time":<epoch>}` | `wh_body` (511) | `{status}` `{time}` `{device}` |
| Webhook GET | gövde yok; URL sabit | — | yok |
| MQTT | `{"state":"alarm\|triggered\|test","device":"LS-…"}` | `mq_payload` (255) | aynı üçü |

Erken uyarı ↔ tetiklenme farkı yalnız `status` değeridir; alarm indeksi (#1/#3) ve kalan süre **taşınmaz**. Dil kavramı yok. GUI yardım metni 6 dilde yer tutucuları açıklar (`GUI/lang/*.json: api.desc, api.body_desc`).

### 4.5 Kalıcı veri — NVS namespace `ls_api`

| Anahtar | Tip | Sınır | Varsayılan | Yazan | Okuyan |
|---|---|---|---|---|---|
| `wh_url` | str | 255 | boş (kapalı) | `save_config c:68` | `init c:36` |
| `wh_method` | str | 7 | `POST` | `c:69` | `c:37` |
| `wh_headers` | str | 255 (JSON nesnesi) | boş | `c:70` | `c:38` |
| `wh_body` | str | 511 | boş (varsayılan gövde) | `c:71` | `c:39` |
| `mq_url` | str | 127 (`mqtt://` / `mqtts://`) | boş | `c:72` | `c:40` |
| `mq_topic` | str | 127 | boş | `c:73` | `c:41` |
| `mq_user` | str | 63 | boş | `c:74` | `c:42` |
| `mq_pass` | str | 63 | boş | `c:75` | `c:43` |
| `mq_payload` | str | 255 | boş (varsayılan yük) | `c:76` | `c:44` |

İlişkili: `ls_actions.early_api`, `trig_api` (u8, varsayılan 0). `mq_pass` GUI'ye `has_pass` olarak döner (`web_server.c:464`); boş gönderilirse korunur (`web_server.c:497`). `wh_headers` içindeki `Authorization: Bearer …` gibi sırlar GUI'ye **açık** döner (`web_server.c:454,461`) ve NVS'de düz metin.

Boş değerler `nvs_set_str("")` ile yazılır (silinmez; SMTP/WiFi'den farklı).

### 4.6 Güç yönetimi

Yok. `SL_EV_WEBHOOK` işareti.

### 4.7 Bağımlılıklar

`CMakeLists.txt`: `esp_http_client mqtt mbedtls nvs_flash log json ls_util sys_log wifi_manager esp_timer`. Etkin: `CONFIG_MQTT_PROTOCOL_311=y`, `CONFIG_MQTT_TRANSPORT_SSL=y`, WebSocket kapalı (`CONFIG_WS_TRANSPORT=n`, `sdkconfig.defaults:91`), MQTT 5 kapalı.

### 4.8 Gördüğüm sorunlar

1. **`mqtts://` çalışmaz** (bkz. 4.3): `broker.verification.crt_bundle_attach = esp_crt_bundle_attach` eksik.
2. **MQTT retain=0 + anlık bağlantı**: abone o an çevrimdışıysa "triggered" kaybolur; Home Assistant yeniden başlarsa durumu bilmez. Ayrıca sabit client ID (`ESP32_<MAC>`) iki cihazda çakışmaz ama LWT/online durumu yok.
3. **Varsayılan webhook gövdesinde `{time}` tutarsızlığı** (`c:152-153` vs `c:107-108`).
4. **URL şablonlanmaz** → GET ile durum iletilemez.
5. **Yeniden deneme yok**; webhook 10 s + 3 yönlendirme, MQTT 13 s; toplam `notify_all` ≤ ~25 s, sonra bitti.
6. **Test httpd görevinde** (`web_server.c:521-524`): 8 KB yığında TLS (webhook https) ve esp-mqtt istemcisi; GUI ≤ 25 s kilitlenir.
7. **`wh_headers` sırları GUI'ye düz döner** (`web_server.c:454`).
8. **`SL_EV_WEBHOOK` iki kanal için tek olay** (`c:189,249`), analizde arg'a bakmak gerekir.
9. **`api_notify_task` içinde `status` string literal işaretçisi** (`main.c:223,362`) — güvenli (rodata) ama görev adı `alarm_api`/`trig_api` iki farklı yerde sabit.
10. **`expand` `{time}`'da saat dilimi yok**, epoch UTC; yük tüketen taraf için belgelenmeli.

### 4.9 Korunması gereken davranışlar

- [ ] `{status}` değerleri tam olarak `alarm`, `triggered`, `test`; `{time}` senkronsa epoch saniye, değilse çalışma süresi saniye; `{device}` cihaz adı.
- [ ] Webhook: GET/POST/PUT; gövde yalnız POST/PUT; başlıklar JSON nesnesi, string değerler; `Content-Type` verilmediyse `application/json`; 2xx başarı; https bundle doğrulamalı; ≤3 yönlendirme; 10 s.
- [ ] MQTT: `mqtt://host:port`, isteğe bağlı kullanıcı/parola, QoS 1, retain 0, tek yayın sonra bağlantı kapanır; bağlanma 8 s, onay 5 s.
- [ ] Webhook yoksa ve MQTT yoksa ama API eylemi açıksa sys_log WARN (`c:288-290`).
- [ ] Her kanalın sonucu ayrı sys_log satırı (`c:280-286`).
- [ ] `/api/api-config` doğrulaması: webhook URL `http(s)://`, MQTT URL `mqtt(s)://`, yöntem normalizasyonu, boş `mq_pass` korunur.
- [ ] `/api/api-config/test {target: webhook|mqtt}` → "Test gonderildi" / "Gonderim hatasi (<esp_err>)".

### 4.10 Açık sorular

- `mqtts://` gerçekten isteniyor mu? İsteniyorsa bundle bağlanmalı; self-signed broker için ayrı CA alanı gerekir.
- MQTT'de retain istenir mi? (Home Assistant için genelde evet; ama "triggered" kalıcı bayrak gibi davranır.)
- Webhook'ta alarm indeksi / kalan süre / timer toplamı gibi ek alanlar istenir mi?
- Tetiklenme bildirimlerinin **sırası ve önceliği**: önce webhook/MQTT (hızlı), sonra mail (yavaş) mi? Şu an paralel görevler, sıra yok.

---

## 5. Çağıran taraf: `main.c` orkestrası (erken uyarı → tetiklenme)

Bu bölüm, dört modülün hangi koşullarda ve hangi görevden çağrıldığını tek yerde toplar.

**Zamanlayıcı kaynağı:** `timer_manager` 1 s'lik esp_timer (`timer_manager.c:266`). Alarm eşiği aşılınca `s_alarm_cb(idx)` (`:218`), süre dolunca `s_expiry_cb()` (`:227`) — ikisi de **esp_timer görevinde** (3584 B yığın).

**Erken uyarı** `on_alarm_triggered(idx)` (`main.c:212-225`), esp_timer bağlamında:
1. `sys_log_write(WARN, "timer", "Erken uyari #%d/%d tetiklendi (kalan %ld sn)")` — **esp_timer görevinde harici flash'a dosya yazımı** (`sys_log.c:624-628`). `main.c:72`'deki yorum OTA için "esp_timer gorevinden flash'a yazilmaz" der; burada aynı kural çiğneniyor.
2. `alarm_mail` (10 KB, prio 5) → `early_mail` bayrağı → SMTP ayar → `ls_ew_mail` → `smtp_client_send`.
3. `alarm_tg` (8 KB) → `early_tg` → Telegram.
4. `early_api=1` ise `alarm_api` (8 KB) → `notify_all("alarm")`.
Üç görev **aynı anda** başlar: en kötü durumda 3 eşzamanlı TLS oturumu (SMTP elle mbedTLS + 2× esp-tls) + sonra MQTT. `xTaskCreate` dönüşleri **denetlenmez** (`main.c:218-224`): heap yetersizse görev sessizce yaratılmaz, hiçbir log düşmez.

**Tetiklenme** `on_timer_expired()` (`main.c:342-364`), esp_timer bağlamında: `SL_EV_TIMER_EXPIRED`, sys_log WARN (flash), `trig_relay` (varsayılan 1) → röle, SSE, `trig_mail` görevi (10 KB), `trig_api=1` ise `trig_api` görevi. Telegram **yok**.

**`trig_mail`** (`main.c:230-339`): `trig_mail` bayrağı → SMTP ayar → `ls_tg_mail.count` → her grup: `g<i>_subj/body/rcpt`, `/ext/mail/g<i>/` dizininden ≤5 dosya (`opendir/readdir`, `malloc(300)` yol başına) → `rcpt`'i virgülle böl, baştaki boşlukları at → her alıcı için `SL_EV_MAIL_SEND` + `send`/`send_with_attachments` → sys_log INFO/ERR → sonunda özet. Gruplar ve alıcılar **sıralı**, tek görevde.

**Bayraklar** `ls_actions` (u8; `web_server.c:1816-1825`): `early_tg early_mail early_api trig_mail trig_api trig_relay`; varsayılanlar 0,0,0,0,0,**1**. Yani fabrika ayarında yalnız röle çalışır; mail/Telegram/API kullanıcı açmadan **gönderilmez**.

**Erken uyarı sayısı ve zamanı:** `alarm_count` (0..10, varsayılan 3), eşikler `total·(count−fired)/(count+1)` (`timer_manager.c:56-66`; 24 saat/3 alarm → 18 h, 12 h, 6 h kala). Her alarm aynı mail/Telegram/API içeriğini üretir; `alarm_index` yalnız loga gider.

---

## 6. `idf_component.yml` ve `sdkconfig.defaults` (ağ kısmı)

### 6.1 Yönetilen bileşenler

`main/idf_component.yml`: yalnız `espressif/mdns: "*"` → çözümlenen **1.14.0** (`esp_bin/HW/dependencies.lock`). `joltwallet/littlefs 1.23.0` da kilit dosyasında (ext_flash'ın manifestinden; ağ dışı). esp_wifi, esp_netif, lwIP, mbedTLS, esp-tls, esp_http_client, esp-mqtt, cJSON IDF yerleşik.

Sürüm `"*"` sabitlenmemiş: her temiz derlemede mdns'in yeni majör sürümü gelebilir (dependencies.lock esp_bin altında tutulduğu için pratikte sabit; 2611'de kilit dosyası projeyle birlikte olmalı).

### 6.2 `sdkconfig.defaults` — ağ/TLS satırları ve gerekçeleri

| Satır | Ayar | Gerekçe (dosyadaki yorum / commit) |
|---|---|---|
| 20 | `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y` | AP modu |
| 27 | `CONFIG_LWIP_MAX_SOCKETS=16` | "max_open_sockets=13 için 13+3=16" yorumu **eski**: web_server artık 7 (`web_server.c:2614`); 16 yine de SSE (3) + httpd (7) + SMTP + Telegram + webhook + MQTT + mDNS/SNTP için gerekli |
| 30 | `CONFIG_ESP_TLS_USING_MBEDTLS=y` | TLS yığını |
| 33 | `CONFIG_NVS_ENCRYPTION=n` | sırlar düz metin (açık karar) |
| 47 | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096` | WiFi olay işleyicide loglama/dosya yazımı |
| 65 | `-Os` | boyut |
| 70 | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y` | yaygın kök CA'lar (~%50 küçük); Gmail/Outlook/Proton/Telegram/GitHub/Let's Encrypt kapsanır; "SMTP doğrulaması zaten OPTIONAL" notu |
| 72 | `CONFIG_LWIP_IPV6=n` | SMTP `AF_INET`, tüm servisler IPv4; mDNS A kaydı |
| 74-75 | `CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y`, sunucu bileti kapalı | HTTPS sunucu yok |
| 77-89 | yeniden pazarlık, PEM yazma, CRL/CSR, Brainpool/Koblitz/192-224 bit eğriler kapalı | boyut; P-256/P-384/P-521/X25519 korunur |
| 91 | `CONFIG_WS_TRANSPORT=n` | MQTT yalnız `mqtt://`/`mqtts://` |
| 93-95 | mDNS browse/CLI/çoklu örnek kapalı | yalnız duyuru |

Bilerek **korunanlar** (hafıza notu `project-firmware-size-decisions`): WPA3/SAE (WPA3-only router'lar), assert ve mbedTLS hata metinleri, `esp_err_to_name`. Bunları yeniden önermemek gerekiyor.

### 6.3 Etkin değerler (`esp_bin/HW/sdkconfig`) — ağ açısından önemli

- WiFi: `ENABLE_WPA3_SAE=y`, `ENABLE_SAE_H2E/PK=y`, `OWE_STA=y`, `ENTERPRISE_SUPPORT=y` (kullanılmıyor), `NVS_ENABLED=y` (sürücü STA kimliğini kendisi de saklar), statik RX 10 / dinamik RX 32 / dinamik TX 32, AMPDU açık, `SOFTAP_BEACON_MAX_LEN=752`, PHY max TX 20 dBm.
- lwIP: DHCP sunucu 8 istasyon (AP 4 ile sınırlı), DNS 3 sunucu, TCP MSS 1440, pencere/gönderim 5760, `TCP_SYNMAXRTX=12`, `TCP_MAXRTX=12`, `SNTP_MAX_SERVERS=1`, SNTP güncelleme 1 saat, başlangıç gecikmesi ≤5 s, `TCPIP_TASK_STACK_SIZE=3072`, `TCPIP_TASK_PRIO=18`.
- mbedTLS: yalnız **TLS 1.2** (`SSL_PROTO_TLS1_3` kapalı), `SSL_IN_CONTENT_LEN=16384`, `OUT=4096` (asimetrik), bundle ≤200 sertifika, donanım AES/SHA/MPI/ECC, ECDHE-ECDSA/RSA, istemci oturum bileti açık (kullanılmıyor: her oturum sıfırdan).
- esp-tls: `CONFIG_ESP_TLS_INSECURE` kapalı → doğrulama seçeneği verilmeyen her esp-tls bağlantısı **reddedilir** (mqtts bulgusunun temeli).
- MQTT: 3.1.1, SSL taşıma var, WS yok, MQTT 5 yok.
- mDNS: görev prio 1, yığın 4096, ≤10 servis, AP+STA+ETH netif önceden tanımlı.
- HTTP istemci: HTTPS açık, olay zaman aşımı 2 s.
- Sistem: `MAIN_TASK_STACK=3584`, `ESP_TIMER_TASK_STACK=3584`, olay kuyruğu 32, TWDT 10 s panik (`sdkconfig.defaults:38-40`), brownout dedektörü açık (`:45`).

---

## 7. Modüller arası ortak sorunlar

1. **İki TLS yığını, üç kurulum:** SMTP elle mbedTLS (`smtp_client.c:289-357`, iki kopya), Telegram/webhook esp-tls (`esp_http_client` + bundle), MQTT esp-tls (bundle **eksik**). Üç farklı doğrulama düzeyi: SMTP yok, Telegram/webhook zorunlu, mqtts çalışmıyor. 2611'de tek bir "güvenli bağlantı" yardımcı katmanı (esp-tls + bundle + ortak zaman aşımı + doğrulama politikası) hepsini kapsar; SMTP de `esp_tls_conn_new_sync`/`esp_tls_plain_tcp_connect` + `esp_tls_set_tls_config` ile STARTTLS yapabilir (IDF'in SMTP örneği bu yolu kullanır).
2. **Yeniden deneme/kuyruk yok:** Geri sayım dolduğunda WiFi yoksa (router kapalı, elektrik kesintisi sonrası router cihazdan geç açıldı) `trig_mail` SMTP'de DNS/TCP hatasıyla biter, timer `enabled=false` olur, bir daha **hiç** denenmez. Dead man's switch'in ana vaadi ("teslimat garantisi", README) bu akışta yerine gelmez. En önemli mimari boşluk.
3. **esp_timer bağlamında flash yazımı ve görev yaratma** (`main.c:216,348` sys_log_write; `218-224,358,362` xTaskCreate) — 3584 B yığın, dönüşler denetlenmez.
4. **Eşzamanlı 3 TLS oturumu + 26 KB görev yığını** erken uyarı anında; heap minimumu ölçülmemiş (`/api/system/detail heap_min` ile izlenebilir).
5. **Testler httpd'de koşar** — tüm GUI kilitlenir (SMTP: sınırsız; Telegram 10 s; webhook 10 s + yönlendirme; MQTT 13 s).
6. **Sabit Türkçe metinler cihazda** (Telegram varsayılanı, test mesajları, SMTP test gövdesi, `From` görünen adı) 6 dilli GUI ile uyumsuz; kalan her şey kullanıcı metni (dil nötr).
7. **Sırların yaşam döngüsü:** WiFi parolaları (2 kopya), SMTP parolası, Telegram token, MQTT parolası, webhook başlıkları — hepsi düz metin NVS; fabrika sıfırlama hepsini siler; `clear_primary` sürücü kopyasını silmez. GUI'ye dönmeyenler: SMTP parola, Telegram token, MQTT parola. Dönen: webhook başlıkları, WiFi SSID'leri (parolalar dönmez).
8. **Zaman aşımı felsefesi tutarsız:** SMTP komut başına 15 s/üst sınır yok; HTTP 10 s toplam; MQTT 8+5 s. Toplam "bildirim turu" süresi hesaplanamıyor.
9. **sys_log olay sayısı:** mail başına `SL_EV_MAIL_SEND` + 1-2 satır; 10 grup × 8 alıcı = 80+ flash satırı tetiklenme anında (64 KB dönen log, `cea47c8`).

---

## 8. Korunacak davranışlar — toplu kontrol listesi

Modül bölümlerindeki listelerin kullanıcıya görünen özeti; 2611 denetiminde (adım 6) her madde cihazda doğrulanmalı.

1. Açılışta parolasız AP `LS-…` / 192.168.4.1, GUI hemen erişilebilir; kayıtlı ağ varsa arka planda tarama+bağlanma; AP açık kalır.
2. Birincil → yedek → 15 s periyodik tarama; kendi kopardığımız bağlantılar retry tetiklemez.
3. AP yalnız STA bağlıyken kapatılabilir; kalıcı.
4. Statik IP türetme kuralları (mask/gw/dns) ve DHCP yedeği.
5. `ls-…​.local` mDNS; STA bağlanınca SNTP; TX gücü: 8 → 2,5 s → tavan(17) / uyarlanabilir / AP istemcisi gelince ≤14 / 8..20 ayar; modem uyku kapalı.
6. SMTP: 465 doğrudan TLS, diğer STARTTLS, AUTH LOGIN, gönderen "LebensSpur <user>", test maili kendine; GUI parola göstermez, boş parola korunur.
7. Erken uyarı: tek adres maili + Telegram (şablon/varsayılan metin) + API ("alarm"), her biri kendi `ls_actions` bayrağıyla; varsayılan hepsi kapalı.
8. Tetiklenme: röle (varsayılan açık) + grup mailleri (sıralı, alıcı başına oturum, ekler) + API ("triggered"); Telegram yok.
9. Webhook GET/POST/PUT + JSON başlıklar + şablon; MQTT QoS1/retain0 tek yayın; `{status} {time} {device}`.
10. Tüm sonuçlar sys_log'a ("mail", "tg", "api", "wifi" kategorileri) ve GUI log görünümüne.
11. REST uçları ve JSON alan adları (`/api/wifi/*`, `/api/smtp/*`, `/api/telegram/*`, `/api/api-config*`, `/api/early-mail/*`, `/api/trigger-mail/*`, `/api/actions/*`) — GUI bunlara bağlı; alan adları `web_server.c:2548-2585` rota tablosunda.

---

## 9. Açık sorular — toplu

1. Teslimat garantisi: tetiklenme anında ağ yoksa kuyruk + yeniden deneme + "teslim edilene kadar" politikası istenir mi? Kaç deneme, hangi aralıkla, kalıcı mı (reboot'u atlatır mı)?
2. AP otomatik geri açılma (STA uzun süre yoksa) istenir mi?
3. SMTP sertifika doğrulaması: zorunlu + istisna anahtarı mı, mevcut gevşek mi?
4. `mqtts://` desteklenecek mi; retain istenir mi?
5. Tetiklenmede Telegram istenir mi?
6. Mail gövdesi 256 karakter ve alıcı listesi 256 karakter sınırları korunacak mı (NVS yerine harici flash'a taşınırsa sınırlar kalkar)?
7. Tek SMTP oturumunda çoklu RCPT (ekleri bir kez gönderme) kabul edilebilir mi?
8. Testlerin httpd dışında ayrı görevde koşup sonucun SSE/polling ile dönmesi kabul mü (GUI akışı değişir)?
9. README'deki AP adı "SmartKraft-LebensSpur" mı, cihaz adı mı?
10. Saat dilimi / `{time}` biçimi (epoch UTC) tüketen sistemler için yeterli mi?
11. Cihaz DHCP hostname'i (`esp_netif_set_hostname`) cihaz adı olsun mu? (Router listesinde tanınabilirlik.)
12. Üç eşzamanlı TLS oturumunun heap etkisi ölçülmeli; gerekirse erken uyarı bildirimleri sıralı yapılmalı.
