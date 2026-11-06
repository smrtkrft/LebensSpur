# 2611 Uygulama Planı (son plan)

> **Ajanlar için:** Bu ana plan her adımın dosyalarını, kesin arayüzlerini ve doğrulamasını verir. Her adım başlamadan hemen önce `04-plan/adim-N.md` olarak tam kodlu, test-önce görev planı yazılır (writing-plans biçimi; önceki adımın gerçek çıktısına göre). Uygulama `subagent-driven-development` ile: görev başına taze alt ajan + iki aşamalı inceleme. Adımlar `- [ ]` ile izlenir.

**Hedef:** ALTE/HW + ALTE/GUI'nin yaptığı her işi yapan, yeni özellik eklemeyen, yalnız dahili flash'ta yaşayan ve harici flash'ı yalnız veri için kullanan yeni LebensSpur firmware'i + web arayüzü.

**Mimari:** İki katman (`platform/`, `app/`), üç görev (`app_core` asla flash dışında bloklamaz, `delivery` seri teslimat, `sys_worker` OTA/depolama); callback'ler yalnız olay bırakır; tek yapılandırma şeması (`cfg_schema.json` → C + GUI); kalıcı outbox; tek erteleme kapısı; API tek zarf; GUI vanilla JS + derleme adımı.

**Teknoloji:** ESP-IDF 5.5.2 (esp32c6), FreeRTOS, esp-tls, esp_http_server (async SSE), esp_https_ota, NVS blob, joltwallet LittleFS 1.23, cJSON (`CJSON_NESTING_LIMIT=16`), espcoredump; GUI: ES2020 + esbuild 0.28.2 (npx, çevrimdışı), Python 3.9 stdlib `build.py`; host testleri Unity 2.6.0 (IDF içinden) + CMake/ctest.

## Küresel kısıtlar

- Kaynak `2611/HW`, `2611/GUI`; üretilen her şey `esp_bin/2611/`; `2611/` altına üretilen dosya yazılmaz.
- Sürüm **yalnız** `2611/HW/CMakeLists.txt` `PROJECT_VER` (başlangıç `1.0.0`); `esp_app_desc.project_name = "lebensspur"`.
- İmaj ≤ 1,5 MiB (`build.sh` durdurur); GUI gömülü toplam ≤ 80 KB gz (`build.py` durdurur: HTML ≤ 13 KB, JS ≤ 14, CSS ≤ 6, dil ×6 ≤ 33, favicon ≤ 1,2).
- Proje bileşenlerinde `-Wall -Wextra -Werror` (`cmake/warnings.cmake`), IDF bileşenleri etkilenmez.
- Çalışma anında görev yaratılmaz; `app/core` saf C (CMake `REQUIRES` beyaz listesi: yalnız `cfg` başlıkları ve `esp_err`).
- Sırlar hiçbir log seviyesinde ve API yanıtında düz görünmez (`"***"`); boş gelen gizli alan eskiyi korur.
- Her ağ işleminde son tarih (deadline): DNS ≤ 21 s ayrı, bağlantı 10 s, el sıkışma 15 s, SMTP gönderim 120 s + ek başına 60 s, HTTP istemci işlem başına 10 s.
- Commit mesajları Türkçe; yazar yalnız kullanıcının git kimliği; her görev ayrı commit; her adım sonunda USB cihaz flash'lanır ve açılış logu alınır.
- Korunan ALTE sdkconfig kararları: WPA3/SAE açık, assert/mbedTLS hata metinleri açık, `esp_err_to_name` açık, tam printf, `-Os`, yaygın CA paketi, IPv6 kapalı, TLS yalnız istemci, TWDT 10 s + PANIC, brownout, USB Serial/JTAG konsol.
- Dil: kod yorumları ve belgeler Türkçe (diyakritikli); C tanımlayıcıları İngilizce.

---

## 1. Dosya haritası

### 1.1 `2611/HW`

| Dosya | Sorumluluk |
|---|---|
| `CMakeLists.txt` | `PROJECT_VER`, `EXTRA_COMPONENT_DIRS components/platform components/app`, `include(cmake/warnings.cmake)` |
| `cmake/warnings.cmake` | proje bileşenlerine `-Wall -Wextra -Werror -Wshadow -Wundef -Wconversion` işlevi `ls_strict_warnings(<target>)` |
| `partitions.csv` | `nvs 0x9000 0x10000` · `otadata 0x19000 0x2000` · (20 KB boşluk) · `ota_0 0x20000 0x1E0000` · `ota_1 0x200000 0x1E0000` · `coredump 0x3E0000 0x20000` |
| `sdkconfig.defaults` | ALTE seti + `03-arastirma.md` §3 "sdkconfig eklemeleri" |
| `main/main.c` | `app_main`: açılış sırası §3.4 (düzeltilmiş: röle pini NVS'ten önce) |
| `main/CMakeLists.txt` | `REQUIRES` tüm bileşenler |
| `tools/gen_cfg.py` | `cfg_schema.json` → `cfg_schema.gen.h/.c` + `schema.gen.json` (derleme dizini `build/gen/cfg/`; configure adımında, şema değişince yeniden) |
| `tools/semver.py` | `build.sh` için sürüm doğrulama |
| `components/platform/cfg/` | `cfg_schema.json`, `cfg.c`, `cfg_nvs.c`, `cfg_json.c`, `cfg_backup.c`, `include/cfg.h`, `include/cfg_schema.gen.h` (üretilen) |
| `components/platform/evlog/` | `evlog.c` (RAM halkası, RTC kaydı, sınıflandırma), `evlog_file.c` (JSONL yazma/okuma/döndürme), `sys_restart.c`, `include/evlog.h` |
| `components/platform/storage/` | `storage.c` (SPI flash, 2 bölüm, mount politikası, durum makinesi, kilit), `include/storage.h` |
| `components/platform/sys_worker/` | `sys_worker.c`, `include/sys_worker.h` |
| `components/platform/jobs/` | `jobs.c` (iş kayıt defteri: id, state, pct, msg), `include/jobs.h` |
| `components/platform/net_wifi/` | `net_wifi.c` (durum makinesi), `net_wifi_tx.c` (TX gücü), `net_wifi_sntp.c`, `include/net_wifi.h` |
| `components/platform/net_tls/` | `net_tls.c` (esp-tls sarmalayıcı, STARTTLS, deadline okuma/yazma, düz TCP), `include/net_tls.h` |
| `components/platform/ota/` | `ota.c` (akış), `semver.c` (saf), `include/ota.h`, `include/semver.h` |
| `components/platform/http/` | `http.c` (httpd, joker rota, ön işleyici, zarf, statik), `http_auth.c` (parola, oturum, IP kilidi), `http_sse.c`, `include/http.h`; GUI gömme CMake |
| `components/app/core/` | `core.c` (görev, kuyruk, olay işleme, status), `timer_sm.c`, `relay_sm.c`, `warn_sched.c`, `button_sm.c`, `outbox.c` (saf politika), `rt_store.c` (NVS `rt` blob), `include/core.h`, `include/core_events.h`, `include/timer_sm.h`, `include/relay_sm.h`, `include/warn_sched.h`, `include/button_sm.h`, `include/outbox.h` |
| `components/app/delivery/` | `delivery.c` (görev, iş alma, deadline bekçisi, sonuç olayı), `include/delivery.h` |
| `components/app/channels/` | `channel.c` (dağıtıcı), `ch_smtp.c`, `smtp_proto.c` (saf: yanıt ayrıştırma, dot-stuffing, base64 satır, RFC 2047), `ch_telegram.c`, `ch_webhook.c`, `ch_mqtt.c`, `mqtt_pkt.c` (saf: paket kurucu), `include/channel.h`, `include/smtp_proto.h`, `include/mqtt_pkt.h` |
| `components/app/io/` | `io_button.c`, `io_relay.c`, `include/io_button.h`, `include/io_relay.h` |
| `components/app/api/` | `api_routes.c` (tablo), `api_auth.c`, `api_status.c`, `api_timer.c`, `api_config.c`, `api_groups.c`, `api_wifi.c`, `api_test.c`, `api_remote.c`, `api_device.c`, `api_ota.c`, `include/api.h` |
| `test/host/CMakeLists.txt` | Unity (IDF'ten), shim, her saf modül için `add_executable` + `add_test` |
| `test/host/shim/esp_log.h`, `shim/sdkconfig.h` | host shim |
| `test/host/test_*.c` | `timer_sm, relay_sm, warn_sched, button_sm, outbox, semver, cfg_json, smtp_proto, mqtt_pkt, evlog_classify` |

### 1.2 `2611/GUI`

| Dosya | Sorumluluk |
|---|---|
| `build.py` | esbuild çağrısı, HTML minify (regex), i18n doğrulama + `keys.json`/dizi üretimi, sprite satır içi, `?v=`/hash, gzip, bütçe denetimi, `gui_hash.txt` |
| `budget.json` | gz bütçeleri |
| `src/index.html` | iskelet, `data-i18n`, sprite `<symbol>`ler |
| `src/css/app.css` | değişkenler, tema `[data-theme=light]`, bileşenler |
| `src/js/main.js`, `api.js`, `i18n.js`, `state.js`, `ui/{form,modal,toast,confirm}.js`, `pages/{home,timer,connections,actions,system,info}.js`, `schema.gen.json` (üretilen) | uygulama |
| `src/lang/{tr,en,de,es,fr,it}.json` | iç içe JSON kaynak (derlemede dönüştürülür) |
| `src/icons/*.svg`, `src/logo.svg`, `src/favicon.svg`, `src/favicon-32.png` | görseller |

### 1.3 `esp_bin`

| Dosya | Sorumluluk |
|---|---|
| `esp_bin/2611/CMakeLists.txt`, `esp_bin/2611/sdkconfig.esp_bin` | sarmalayıcı (ALTE kalıbı), bölüm tablosu yolu |
| `esp_bin/build.sh` | varsayılan 2611; `--alte`; `test` alt komutu; 1,5 MiB eşiği; `esp_idf_size` JSON günlüğü; firmware kopyası `lebensspur_v<ver>.bin` + `.elf` |
| `esp_bin/test.sh` | `SDKROOT` + CMake/ctest host testleri |

---

## 2. Arayüzler (kesin imzalar)

Aşağıdaki başlıklar görevler arası sözleşmedir; uygulayan ajan yalnız kendi görevini görür.

### 2.1 `cfg.h`

```c
#include "cfg_schema.gen.h"   // cfg_t, cfg_group_t, cfg_section_t, CFG_SEC__COUNT, cfg_field_t tablo
esp_err_t   cfg_init(void);                                   // NVS; meta yoksa nvs_flash_erase + varsayılan; evlog
const cfg_t *cfg_get(void);                                   // RAM kopyası, yalnız okuma
esp_err_t   cfg_set_section_json(cfg_section_t s, const cJSON *obj, char *err, size_t errlen, const char **errfield);
cJSON      *cfg_get_section_json(cfg_section_t s, bool mask_secrets);
esp_err_t   cfg_group_get(int idx, cfg_group_t *out);         // 0..9; yoksa ESP_ERR_NOT_FOUND
esp_err_t   cfg_group_set_json(int idx, const cJSON *obj, char *err, size_t errlen, const char **errfield);
esp_err_t   cfg_group_delete(int idx);
int         cfg_group_count(void);
esp_err_t   cfg_factory_reset(void);                          // kademe 1: tüm blob'lar + rt; meta yeniden
esp_err_t   cfg_backup_write(void);                           // sys_worker: /data/config JSONL
esp_err_t   cfg_backup_info(cfg_backup_info_t *out);          // var mı, seq, schema
esp_err_t   cfg_backup_restore(void);                         // sys_worker
typedef void (*cfg_change_cb_t)(cfg_section_t s);
void        cfg_set_change_cb(cfg_change_cb_t cb);            // core → EV_CFG_CHANGED
```

`cfg_schema.json` biçimi: `{"sections":{"smtp":{"blob":"smtp","fields":[{"name":"server","type":"str","len":63},{"name":"port","type":"u16","min":1,"max":65535,"default":465},{"name":"pass","type":"str","len":63,"secret":true},{"name":"tls_verify","type":"bool","default":true}]},…},"group":{...}}`. Tipler: `str(len)`, `bool`, `u8/u16/u32(min,max)`, `enum(values)`. Üretilen `cfg_t` bölüm yapıları + `cfg_field_t { section, name, type, offset, len, min, max, secret, defstr/defnum, enum }` tablosu + `_Static_assert(sizeof(section) <= 4000)`.

### 2.2 `evlog.h`

```c
typedef enum { EVL_INFO, EVL_WARN, EVL_ERR } evlog_level_t;
typedef enum { RST_GENERIC, RST_USER_WEB, RST_USER_BUTTON, RST_FACTORY, RST_OTA, RST_DELIVERY_HANG } restart_reason_t;
typedef enum { EVM_BOOT, EVM_AP_CLIENT_JOIN, EVM_AP_CLIENT_LEAVE, EVM_STA_CONNECTING, EVM_STA_GOT_IP, EVM_STA_LOST,
               EVM_GUI_SERVE, EVM_MAIL_SEND, EVM_TG_SEND, EVM_WEBHOOK, EVM_MQTT, EVM_OTA, EVM_TX_POWER, EVM_RELAY,
               EVM_TIMER_EXPIRED, EVM_BUTTON, EVM_CHECKIN } evlog_mark_t;
void      evlog_init(void);                        // RTC sağlama, önceki kapanış sınıflandırması, boot kaydı halkaya
void      evlog_write(evlog_level_t lvl, const char *cat, const char *fmt, ...) __attribute__((format(printf,3,4)));
void      evlog_mark(evlog_mark_t ev, int32_t arg); // RTC son olay (ucuz)
void      evlog_set_wifi_ctx(uint8_t mode, uint8_t clients, int8_t rssi, uint8_t tx_q);
void      sys_restart(restart_reason_t r) __attribute__((noreturn));
esp_err_t evlog_flush(void);                       // sys_worker: halka → /log/events.jsonl (fsync)
esp_err_t evlog_read(uint32_t before_n, uint32_t before_up, int limit, cJSON **out_array); // httpd
esp_err_t evlog_clear(void);
typedef struct { uint32_t boot_count, short_boots, prev_uptime_s; char cat[12], cause[20]; bool approx, coredump; } evlog_boot_info_t;
void      evlog_boot_info(evlog_boot_info_t *out);
```

Sınıflandırma `evlog_classify(esp_reset_reason_t, bool rtc_valid, restart_reason_t)` saf fonksiyon (host testi) → `(cat, cause)`.

### 2.3 `storage.h`

```c
typedef enum { STG_ABSENT, STG_MOUNTED, STG_ERROR, STG_FORMATTING } storage_state_t;
#define STG_LOG  "/log"
#define STG_DATA "/data"
esp_err_t       storage_init(void);                  // SPI2, çip boyutu, lslog(1 MB@0) + lsdata(kalan), mount politikası
storage_state_t storage_state(const char *mount);
bool            storage_acquire(const char *mount);  // okuma/yazma sayacı; ERROR/FORMATTING ise false
void            storage_release(const char *mount);
esp_err_t       storage_format(const char *mount);   // dışlayıcı; sayaç 0 bekler (≤5 s) → ESP_ERR_TIMEOUT
esp_err_t       storage_erase_data_full(void (*progress)(int pct)); // kademe 2: erase_range + format; /log korunur
esp_err_t       storage_info(const char *mount, size_t *total, size_t *used);
uint32_t        storage_chip_size(void);
```

### 2.4 `sys_worker.h`, `jobs.h`

```c
typedef void (*sys_job_fn_t)(void *arg);
esp_err_t sys_worker_start(void);
esp_err_t sys_worker_post(sys_job_fn_t fn, void *arg);   // kuyruk 8; dolu → ESP_ERR_NO_MEM (+ evlog q_drop)

typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED } job_state_t;
typedef uint16_t job_id_t;
job_id_t  jobs_create(const char *kind);                  // 8 kayıt, LRU
void      jobs_update(job_id_t id, job_state_t st, int pct, const char *msg);
cJSON    *jobs_get_json(job_id_t id);                     // NULL → yok
```

### 2.5 `net_wifi.h`, `net_tls.h`

```c
typedef enum { WST_AP_ONLY, WST_SCANNING, WST_CONNECTING, WST_CONNECTED, WST_DISCONNECTING, WST_RETRY_WAIT } wifi_state_t;
typedef enum { WERR_NONE, WERR_AUTH, WERR_NOT_FOUND, WERR_SECURITY, WERR_OTHER } wifi_err_class_t;
typedef struct { wifi_state_t state; char ip[16]; char ssid[33]; int8_t rssi; uint8_t ap_clients; uint8_t tx_now_q;
                 bool ap_enabled, ap_auto_opened, using_backup; struct { wifi_err_class_t cls; int reason; int8_t rssi; } last_error; } wifi_status_t;
typedef void (*net_wifi_link_cb_t)(bool up);
esp_err_t net_wifi_start(const char *device_name, net_wifi_link_cb_t cb);
void      net_wifi_get_status(wifi_status_t *out);
esp_err_t net_wifi_reconnect(void);           // cfg wifi_p/wifi_b yeniden yüklenir
esp_err_t net_wifi_set_ap_enabled(bool on);   // STA bağlı değilken kapatma → ESP_ERR_INVALID_STATE
esp_err_t net_wifi_apply_tx(void);            // cfg wifi_tx değişince
void      net_wifi_set_tx_cap_q(uint8_t q);   // brownout açılışı: geçici tavan (0 = kaldır)

typedef struct { bool verify; const char *cacert_pem; uint32_t connect_ms; uint32_t handshake_ms; } net_tls_opts_t;
int        net_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms);   // fd ya da -1 (errno)
esp_tls_t *net_tls_connect(const char *host, uint16_t port, const net_tls_opts_t *o);
esp_tls_t *net_tls_starttls(int fd, const char *host, const net_tls_opts_t *o);      // mevcut soket üzerinde
int        net_tls_read(esp_tls_t *t, void *buf, size_t len, int64_t deadline_us);   // >0 bayt, 0 kapandı, <0 hata/zaman aşımı
int        net_tls_write_all(esp_tls_t *t, const void *buf, size_t len, int64_t deadline_us);
int        net_tls_readline(esp_tls_t *t, char *line, size_t cap, int64_t deadline_us); // CRLF'e kadar
void       net_tls_close(esp_tls_t *t);
```

### 2.6 `ota.h`, `semver.h`

```c
typedef enum { OTA_IDLE, OTA_CHECKING, OTA_AVAILABLE, OTA_NO_UPDATE, OTA_DOWNLOADING, OTA_DONE, OTA_ERROR, OTA_ABORTED } ota_state_t;
typedef struct { ota_state_t state; int pct; char message[96]; char current[32], remote[32], pending[32], rollback[32];
                 bool rollback_available, pending_verify; } ota_status_t;
esp_err_t ota_init(void);                                   // pending_verify, last_invalid → evlog
void      ota_get_status(ota_status_t *out);
esp_err_t ota_check(bool custom, const char *url);          // sys_worker işi; HEAD → tag ya da özel URL
esp_err_t ota_start(bool allow_downgrade);                  // sys_worker işi; img_desc kapısı
void      ota_abort(void);                                  // atomic bayrak
esp_err_t ota_rollback(void);
void      ota_confirm_if_pending(void);                     // sys_worker işi
int       semver_cmp(const char *a, const char *b);         // saf; -1/0/1; "v" öneki ve -pre kabul
```

### 2.7 `http.h`

```c
typedef struct http_req http_req_t;
typedef esp_err_t (*http_handler_t)(http_req_t *r);
enum { HR_AUTH = 1, HR_CSRF = 2, HR_BODY = 4 };
typedef struct { const char *pattern; httpd_method_t method; uint8_t flags; http_handler_t fn; } http_route_t; // "/api/groups/{i}/files/{name}"
esp_err_t   http_start(const http_route_t *routes, size_t n);
esp_err_t   http_ok(http_req_t *r, cJSON *data);                                  // data sahipliği alınır, NULL = {}
esp_err_t   http_fail(http_req_t *r, int status, const char *code, const char *msg, const char *field);
const cJSON *http_body(http_req_t *r);                                            // HR_BODY ile parse edilmiş
const char *http_param(http_req_t *r, int idx);                                   // {i}, {name}
int         http_param_int(http_req_t *r, int idx, int def);
const char *http_query(http_req_t *r, const char *key, char *buf, size_t cap);
esp_err_t   http_stream_begin(http_req_t *r, const char *ctype);                  // chunked
esp_err_t   http_stream_write(http_req_t *r, const void *d, size_t n);
esp_err_t   http_stream_end(http_req_t *r);
int         http_body_fd(http_req_t *r, size_t *content_len);                     // ham gövde (dosya yükleme)
int         http_body_recv(http_req_t *r, void *buf, size_t cap);
void        http_sse_broadcast(void);                                             // her görevden; status push
bool        http_auth_enabled(void);
const char *http_client_ip(http_req_t *r);
```

### 2.8 `core.h`, `core_events.h`, saf durum makineleri

```c
typedef enum { CHK_BUTTON, CHK_WEB, CHK_API_KEY } checkin_source_t;
typedef enum { EV_TICK, EV_BUTTON_PRESS, EV_BUTTON_WINDOW, EV_CHECKIN, EV_TIMER_SET, EV_TIMER_CANCEL, EV_VACATION_SET,
               EV_VACATION_CANCEL, EV_RELAY_TEST, EV_DELIVERY_RESULT, EV_NET_LINK, EV_CFG_CHANGED, EV_OTA_STATE,
               EV_FACTORY_RESET } core_event_type_t;
typedef struct { core_event_type_t type;
  union { struct { checkin_source_t src; } checkin;
          struct { uint8_t unit, value, warn; } timer_set;      // unit: 0 dk, 1 saat, 2 gün
          struct { uint8_t days; } vacation;
          struct { uint16_t job_seq; uint8_t result; char err[48]; } delivery;   // result: channel_result_t
          struct { bool up; } link;
          struct { uint8_t section; } cfg;
          struct { uint8_t level; } factory; } u; } core_event_t;
typedef enum { TS_IDLE, TS_RUNNING, TS_VACATION, TS_STOPPED, TS_TRIGGERED } timer_state_t;
typedef struct { timer_state_t state; uint32_t total_s, remaining_s, vac_total_s, vac_remaining_s; uint8_t warn_count, warn_fired;
                 uint8_t relay_state; uint32_t relay_remaining_s; uint8_t pending_total, pending_sent, pending_failed;
                 uint32_t since_trigger_s; checkin_source_t last_checkin_src; uint32_t last_checkin_up_s; } app_status_t;
esp_err_t core_start(void);
esp_err_t core_post(const core_event_t *ev);            // 0 bekleme; dolu → ESP_ERR_NO_MEM + q_drop
esp_err_t core_post_from_isr(const core_event_t *ev);
void      core_get_status(app_status_t *out);
```

Saf modüller (IDF'siz; `now_us` enjekte):

```c
// timer_sm.h
typedef struct { timer_state_t state; int64_t deadline_us, vac_deadline_us; uint32_t total_s, vac_total_s, remaining_at_stop_s;
                 uint8_t warn_count, warn_fired; } timer_sm_t;
typedef enum { TA_NONE=0, TA_WARN=1, TA_TRIGGER=2, TA_VACATION_END=4, TA_PERSIST=8 } timer_action_t;
void     timer_sm_init(timer_sm_t *s, const rt_blob_t *rt, int64_t now_us);          // rt NULL → IDLE
int      timer_sm_set(timer_sm_t *s, uint8_t unit, uint8_t value, uint8_t warn, int64_t now_us, uint8_t *warn_effective);
int      timer_sm_checkin(timer_sm_t *s, int64_t now_us);     // 0 ok, 1 tatilde (etkisiz), -1 no_timer
int      timer_sm_cancel(timer_sm_t *s, int64_t now_us);
int      timer_sm_vacation(timer_sm_t *s, uint8_t days, int64_t now_us);
int      timer_sm_vacation_cancel(timer_sm_t *s, int64_t now_us);
unsigned timer_sm_eval(timer_sm_t *s, int64_t now_us, uint8_t *warn_idx);            // timer_action_t bitleri
uint32_t timer_sm_remaining_s(const timer_sm_t *s, int64_t now_us);
void     timer_sm_to_rt(const timer_sm_t *s, int64_t now_us, rt_blob_t *rt);
// warn_sched.h
uint32_t warn_threshold_s(uint32_t total_s, uint8_t n, uint8_t fired);    // total·(n−fired)/(n+1); 0 = yok
uint8_t  warn_resync(uint32_t total_s, uint32_t remaining_s, uint8_t n);  // açılışta geçmiş eşik sayısı
uint8_t  warn_clamp(uint8_t unit, uint8_t value, uint8_t n);             // ALTE value/2 (ayrı oturumda değişecek)
// relay_sm.h
typedef enum { RS_OFF, RS_DELAY, RS_ON, RS_PULSE_ON, RS_PULSE_OFF } relay_state_t;
typedef struct { bool inverted; uint8_t delay_s, pulse_s; uint16_t duration_s; } relay_cfg_t;
typedef struct { relay_state_t state; int64_t phase_deadline_us, end_deadline_us; relay_cfg_t cfg; } relay_sm_t;
void relay_sm_init(relay_sm_t *r, const relay_cfg_t *c);
void relay_sm_trigger(relay_sm_t *r, int64_t now_us, uint16_t duration_override_s); // 0 = cfg
void relay_sm_off(relay_sm_t *r);
bool relay_sm_eval(relay_sm_t *r, int64_t now_us);          // dönüş: fiziksel çıkış enerjili mi
uint32_t relay_sm_remaining_s(const relay_sm_t *r, int64_t now_us);
// button_sm.h
typedef enum { BA_NONE, BA_CHECKIN, BA_RESTART, BA_FACTORY1 } button_action_t;
typedef struct { uint8_t count; int64_t first_us, last_us; } button_sm_t;
void            button_sm_press(button_sm_t *b, int64_t now_us);
button_action_t button_sm_window_end(button_sm_t *b, int64_t now_us);   // 400 ms sonra
// outbox.h (saf politika)
typedef enum { JK_EARLY_WARN, JK_TRIGGER, JK_TEST } job_kind_t;
typedef enum { CH_SMTP, CH_TELEGRAM, CH_WEBHOOK, CH_MQTT } channel_t;
typedef struct { uint16_t seq; job_kind_t kind; channel_t ch; uint8_t group, rcpt, attempt, warn_idx; int64_t not_before_us; uint8_t state; } job_t;
#define OUTBOX_MAX 48
typedef struct { job_t jobs[OUTBOX_MAX]; uint16_t next_seq; uint32_t trigger_seq; int64_t trigger_at_us; } outbox_t;
void  outbox_init(outbox_t *o);
int   outbox_plan_trigger(outbox_t *o, const cfg_actions_t *a, const uint8_t *group_rcpt_counts, int groups, int64_t now_us);
int   outbox_plan_early(outbox_t *o, const cfg_actions_t *a, uint8_t warn_idx, int64_t now_us);
int   outbox_plan_test(outbox_t *o, channel_t ch, int64_t now_us);
const job_t *outbox_next(const outbox_t *o, int64_t now_us, bool link_up);  // öncelik TRIGGER>EARLY>TEST, not_before
void  outbox_on_result(outbox_t *o, uint16_t seq, uint8_t result, int64_t now_us, uint32_t (*rand_fn)(void));
int   outbox_cancel_pending_trigger(outbox_t *o);           // erteleme: başlamamış tetikleme işleri
int64_t outbox_retry_delay_us(job_kind_t k, uint8_t attempt, uint32_t rnd);  // 0/1/5/15/30 dk → 30 dk ±%10; EARLY 0/1/5
bool  outbox_should_give_up(const outbox_t *o, const job_t *j, int64_t now_us); // 72 sa açık süre
void  outbox_to_rt(const outbox_t *o, rt_blob_t *rt); void outbox_from_rt(outbox_t *o, const rt_blob_t *rt, int64_t now_us);
```

`rt_blob_t` (≤ 64 B): `{uint8_t schema, state, warn_fired, relay_dummy; uint32_t total_s, remaining_s, vac_total_s, vac_remaining_s, trigger_seq, since_trigger_s; uint8_t pending[10]; /* bit0..3 kanal; */ uint8_t attempts[10]; uint8_t early_pending; uint16_t seq;}`.

### 2.9 `channel.h`, `delivery.h`

```c
typedef enum { CR_OK, CR_RETRY, CR_PERM, CR_CONFIG } channel_result_t;
channel_result_t channel_send(const job_t *j, char err[96]);     // delivery görevinde; SMTP/TG/WH/MQTT dağıtır
esp_err_t delivery_start(void);
void      delivery_kick(void);                                    // app_core: outbox'ta iş var
// delivery görevi core'a EV_DELIVERY_RESULT gönderir; app_core her iş için deadline tutar (ch_smtp: 120 s + 60 s×ek)
```

### 2.10 `io_button.h`, `io_relay.h`, `api.h`

```c
esp_err_t io_button_init(void);           // GPIO18 pull-up, NEGEDGE ISR, 200 ms debounce → core_post_from_isr(EV_BUTTON_PRESS)
void      io_relay_early_init(bool inverted_hint); // app_main ilk satırı: GPIO19 çıkış, boşta seviyesi (gpio_reset_pin YOK)
void      io_relay_set(bool energized, bool inverted);
const http_route_t *api_routes(size_t *count);
```

---

## 3. Adımlar ve görevler

Her görev: **Dosyalar · Üretir/Tüketir · Test (host/cihaz) · Commit**. Tam kod `04-plan/adim-N.md`'de.

### Adım 0 — İskelet ve derleme hattı

- [ ] **0.1 Sarmalayıcı ve build.sh**: `esp_bin/2611/CMakeLists.txt` (ALTE kalıbı, `SRC_DIR=2611/HW`), `esp_bin/2611/sdkconfig.esp_bin`, `esp_bin/build.sh` (varsayılan 2611, `--alte`, `test`, 1,5 MiB eşiği, `.elf` kopyası). Test: `./esp_bin/build.sh --alte` eski ağacı aynen derler. Commit `build: esp_bin 2611 sarmalayıcı, build.sh ağaç seçimi`.
- [ ] **0.2 HW iskeleti**: `2611/HW/{CMakeLists.txt,partitions.csv,sdkconfig.defaults,cmake/warnings.cmake,main/main.c}` + boş `platform/`, `app/` bileşen dizinleri (her biri `idf_component_register` + `ls_strict_warnings`). `main.c`: sürüm logu, `heap_caps_get_info` tablosu (toplam/boş/en büyük blok). Cihaz: `erase-flash` (tek seferlik, tablo değişimi), `flash`, açılış logu; imaj boyutu kaydı. Commit `feat(2611): HW iskeleti, bölüm tablosu, sdkconfig`.
- [ ] **0.3 Host test hattı**: `esp_bin/test.sh` (`SDKROOT`), `2611/HW/test/host/{CMakeLists.txt,shim/}`, örnek `test_smoke.c`. Test: `./esp_bin/build.sh test` → "100% tests passed". Commit `test: masaüstü Unity hattı`.
- [ ] **0.4 cfg üreteci**: `tools/gen_cfg.py` + `cfg_schema.json` (tam şema §5.1 + `hidden`, `ap_pass`, `tx_max` q-değeri, `tls_verify`) → `cfg_schema.gen.h/.c`, `GUI/src/schema.gen.json`; CMake `add_custom_command`. Test: host `test_cfg_gen.c` (offset/len tablosu ile struct tutarlılığı, `_Static_assert`). Commit `feat(cfg): şema üreteci`.
- [ ] **0.5 GUI derleme hattı**: `2611/GUI/{build.py,budget.json,src/index.html(min),src/js/main.js(min),src/css/app.css(min),src/lang/*.json(ALTE'den kopya),icons/,logo.svg,favicon.svg,favicon-32.png}`; `platform/http` CMake'te `add_custom_command` + `EMBED_FILES` + `gui_hash.txt`. Test: derleme, `build.py` bütçe raporu, i18n doğrulama (bilerek bozuk anahtarla durma). Commit `build(gui): esbuild/i18n/sprite/gzip hattı`.

### Adım 1 — Platform temeli: cfg, evlog, storage, sys_worker, jobs

- [ ] **1.1 `cfg` çekirdeği**: `cfg.c` (RAM, varsayılanlar, meta/erase kuralı), `cfg_nvs.c` (blob `{schema,len}`), `cfg_json.c` (tablo güdümlü doğrulama, maskeleme, boş-gizli-koru, hata alanı). Host: `test_cfg_json.c` (tür/sınır/enum/secret/round-trip). Cihaz: `main.c` cfg_init + bir alan yaz/oku/yeniden başlat. Commit `feat(cfg): NVS blob, JSON doğrulama`.
- [ ] **1.2 `evlog`**: RTC kaydı, `evlog_classify` (saf, host testi: ALTE tablosu birebir), RAM halkası, `evlog_write` bloklamaz, `sys_restart`, boot kaydı. Cihaz: üç reset türü (buton 3×, `esp_restart`, brownout simülasyonu mümkün değilse WDT) sınıflandırması. Commit `feat(evlog): RTC kaydı, sınıflandırma, bloklamayan yazım`.
- [ ] **1.3 `sys_worker` + `jobs`**: görev (3, 12 KB), kuyruk 8, TWDT abonesi; `jobs` kayıt defteri. Host: `test_jobs.c`. Commit `feat(sys_worker): iş kuyruğu ve iş kayıt defteri`.
- [ ] **1.4 `storage`**: SPI2 (`SPI_DMA_DISABLED`, DIO 40 MHz), çip boyutu `[4,32] MB`, `lslog`/`lsdata` `esp_partition_register_external`, mount politikası (boş 8 KB → format; dolu bozuk → ERROR), `.ls2611` işareti, kilit/sayaç, `storage_erase_data_full` ilerlemeli. Cihaz: mount, bilgi, süre ölçümleri (çip yok senaryosu: CS pinini kaldırmadan simüle edilemez → kod yolu birim testli). Commit `feat(storage): harici flash, iki bölüm, mount politikası`.
- [ ] **1.5 `evlog_file` + `cfg_backup`**: JSONL yazma/`fsync`/döndürme (`EBUSY` erteleme), ters okuma + sayfalama; yedek JSONL+CRC (`tmp→prev→cur`), geri yükleme, 300 ms birleştirme. Host: `test_evlog_file.c` (ayrıştırma), `test_cfg_backup.c` (CRC, satır biçimi). Cihaz: 1000 kayıt + döndürme; yedek yaz/geri yükle. Commit `feat(evlog,cfg): dosya günlüğü ve yapılandırma yedeği`.

### Adım 2 — Uygulama çekirdeği

- [ ] **2.1 Saf durum makineleri**: `warn_sched`, `timer_sm` (son tarih aritmetiği), `relay_sm`, `button_sm`. Host: `test_warn_sched.c` (ALTE örnekleri: 24 s/3 → 18/12/6; resync), `test_timer_sm.c` (tüm geçişler, tatil, tetik, rt gidiş-dönüş, kapalı süre sayılmaz), `test_relay_sm.c` (delay/pulse/duration/override/off), `test_button_sm.c` (1–2/3–4/5). Commit `feat(core): sayaç/röle/düğme durum makineleri (saf, testli)`.
- [ ] **2.2 `outbox` politikası**: planlama, öncelik, yeniden deneme gecikmeleri (jitter), vazgeçme 72 sa, iptal, rt gidiş-dönüş. Host: `test_outbox.c`. Commit `feat(core): outbox politikası`.
- [ ] **2.3 `core` görevi + `rt_store` + `io`**: kuyruk 32, TWDT, 1 Hz esp_timer → `EV_TICK` (`skip_unhandled_events`), olay işleme, `app_status_t`, `rt` yazımı (değişince, süre ölçümü), `io_relay` (erken init), `io_button` (ISR+debounce), `power_quiet_until`, brownout düşük-güç açılışı bayrağı (K5), delivery deadline bekçisi iskeleti. Cihaz: düğme ile erteleme/yeniden başlatma/kademe-1; röle testi; yeniden başlatmada sayaç devamı; `rt` yazım süresi logu. Commit `feat(core): app_core görevi, kalıcılık, düğme ve röle`.

### Adım 3 — Ağ ve HTTP temeli, minimal GUI

- [ ] **3.1 `net_tls`**: düz TCP (deadline connect), esp-tls bağlan, STARTTLS yükseltme, deadline okuma/yazma/satır. Cihaz: Gmail 465 ve 587 el sıkışması (yalnız bağlan-kapat), süre ölçümü. Commit `feat(net_tls): esp-tls sarmalayıcı, STARTTLS`.
- [ ] **3.2 `net_wifi`**: durum makinesi (geçiş tablosu `05-wifi.md` §3), async tarama (dwell), `self_disc`, 1..5 s denemeler, yedek, RETRY_WAIT 15→60 s, `hidden`, `sta.channel` ipucu, AP otomatik açma 10 dk, birincil 30 dk arka plan, `STA_LOST_IP`, hostname, SNTP, mDNS, TX gücü (q-değeri, 9 kademe, RSSI tablosu, histerezis, AP istemcisi ≤56, yumuşak başlangıç, `set_tx_cap_q`), `evlog_set_wifi_ctx`. Cihaz: AP açılışı, Mac'ten bağlanma, STA bağlanma (router menzildeyse), yanlış parola sınıfı, kopma/yeniden bağlanma logları. Commit `feat(net_wifi): olay güdümlü WiFi durum makinesi ve TX gücü`.
- [ ] **3.3 `http` + `http_auth` + `http_sse`**: joker rota + kendi tablo + yol parametreleri, statik `s_ctx`, ön işleyici (auth, CSRF seti, 413/400), zarf, oturum çerezi (bağlamda), IP başına kilit tablosu, SSE async (4 slot, PEEK, kalp atışı), statik gömülü dosyalar (`/`, `/s/<hash>/…`, `/favicon.*`), `ETag` index. Cihaz (Mac → AP): Python sözleşme betiği `2611/HW/test/api/test_api.py` ilk sürüm: 401/CSRF/413/400/derin JSON/`Host: evil`. Commit `feat(http): httpd katmanı, kimlik, CSRF, SSE`.
- [ ] **3.4 `api` ilk küme + minimal GUI**: `api_routes.c`, `api_auth.c`, `api_status.c`, `api_timer.c`, `api_wifi.c` (durum + connect), `api_config.c` (GET/PUT bölümler); GUI: `api.js`, `i18n.js`, `state.js` (SSE + yerel tik + 30 s yedek + visibility), `ui/*`, `pages/home.js`, `pages/timer.js`, giriş ekranı. Cihaz: tarayıcıdan sayaç kur/ertele/durdur, SSE akışı, giriş. Commit `feat(api,gui): durum/sayaç/WiFi uçları ve ana ekran`.

### Adım 4 — Teslimat ve kanallar

- [ ] **4.1 `smtp_proto` + `mqtt_pkt` (saf)**: yanıt ayrıştırma (çok satır), kod sınıfı (`OK/RETRY/PERM/CONFIG`; 421/450/451/452 RETRY), dot-stuffing, CRLF, base64 76/satır (2964→4056), RFC 2047, `Message-ID`, EHLO yetenek ayrıştırma (AUTH PLAIN/LOGIN, SIZE); MQTT CONNECT/PUBLISH(QoS1)/PUBACK/DISCONNECT kurucu/çözücü. Host: `test_smtp_proto.c`, `test_mqtt_pkt.c`. Commit `feat(channels): SMTP ve MQTT protokol yardımcıları (saf, testli)`.
- [ ] **4.2 `ch_smtp`**: 465 doğrudan / diğer STARTTLS, EHLO→AUTH→MAIL→RCPT→DATA→`.`→QUIT, grup başına tek oturum çoklu RCPT, ekler `/data/mail/g<N>/` 2964 B parça, eksik ek notu, `tls_verify`, deadline'lar, sır maskeleme. Cihaz: Gmail uygulama parolasıyla gerçek gönderim (ekli/eksiz), hatalı parola → CONFIG, 550 → PERM. Commit `feat(channels): SMTP istemcisi`.
- [ ] **4.3 `ch_telegram`, `ch_webhook`, `ch_mqtt`**: esp_http_client (`buffer_size_tx 1024`, 2 yönlendirme, close, yanıt gövdesi okuma, `retry_after`), şablon `{status}{time}{device}`, `Idempotency-Key`; MQTT `net_tls` üzerinde tek yayın (`mqtt://`, `mqtts://`), `seq`. Cihaz: gerçek Telegram bot, webhook (ör. `requestbin`/yerel HTTP sunucu), Mosquitto test broker (`test.mosquitto.org` 1883/8883). Commit `feat(channels): Telegram, webhook, MQTT`.
- [ ] **4.4 `delivery` + `channel` + core entegrasyonu + test uçları**: görev (5, 10 KB), `delivery_kick`, sonuç olayı, deadline bekçisi (`RST_DELIVERY_HANG`), `EV_NET_LINK` + 10 s yerleşme, `power_quiet_until`, kanal otomatik açma (test geçince), `api_test.c` (`POST /api/test/<ch>` → job), `api_groups.c` (gruplar + dosya yükleme/listeleme/silme, sınırlar). Cihaz: 2 dk sayaç + 1 uyarı → uyarı maili → tetikleme → grup mailleri + webhook + MQTT; ağ kapalıyken tetikleme → ağ gelince teslimat; yeniden başlatma ortasında devam; erteleme ile kuyruk iptali. Commit `feat(delivery): outbox görevi, kanal entegrasyonu, test uçları`.

### Adım 5 — GUI tamamı

- [ ] **5.1 Connections + Actions sayfaları**: SMTP (yardım metni: Gmail/iCloud uygulama parolası, Outlook.com kişisel desteklenmez), WiFi birincil/yedek (+hidden, statik IP doğrulama), AP (parola alanı), TX 9 kademe; aksiyon kartları, webhook/MQTT, röle (ayarlar yüklenir; döngü özeti), Telegram, erken uyarı maili, mail grupları (ekler, sınırlar, eksik ek göstergesi). Tek kaydetme modeli, kirli form uyarısı. Commit `feat(gui): bağlantılar ve aksiyonlar`.
- [ ] **5.2 System + Info sayfaları**: güvenlik (parola, oturum, kilit), uzaktan anahtar, OTA kartı (adım 6 uçlarıyla), yeniden başlat/fabrika 1–2 (onaylar, ilerleme), yedek geri yükleme teklifi, tema/dil; cihaz bilgisi (heap_largest dahil), depolama durumu/rozet, günlükler (sayfalama, filtreler, boot kayıtları), coredump indir/sil. Commit `feat(gui): sistem ve bilgi`.
- [ ] **5.3 i18n, erişilebilirlik, bütçe kalibrasyonu**: 6 dil (ALTE anahtarları + yeni ekranlar; `replaceAll`), `aria-*`, `<label for>`, klavye/Esc/odak; `budget.json` kalibre edilip sabitlenir (indeks modu gerekirse). Commit `feat(gui): i18n, erişilebilirlik, bütçe`.

### Adım 6 — OTA, coredump, dışa/içe aktarma, fabrika sıfırlama, günlük API

- [ ] **6.1 `semver` + `ota`**: `semver_cmp` (host testi 18 vaka), `HEAD /releases/latest` → tag (`HTTP_EVENT_ON_HEADER`), `esp_https_ota` (`buffer_size_tx 2048`, `timeout 10 s`, `partial` kapalı), `get_img_desc` kapısı (`project_name`, sürüm), abort bayrağı, `PENDING_VERIFY` reddi, koşullu onay (`sys_worker`), rollback hesaplama, `RST_OTA`, `last_invalid` → evlog; `api_ota.c`. Cihaz: `smrtkrft/LebensSpur-firmware` test release'i (ya da özel URL) ile tam tur: kontrol → indir → yeniden başlat → onay; geri alma; tetikleme sırasında abort. Commit `feat(ota): API'siz GitHub OTA, sürüm kapısı, koşullu onay`.
- [ ] **6.2 Coredump + `api_device.c`**: `ESP_COREDUMP` seti, açılışta özet → boot kaydı, `GET/DELETE /api/coredump` (chunked), `GET /api/device` (heap_largest, yığın su izleri), restart, factory-reset `{level}` (1: cfg; 2: iş + ilerleme + `factory2_pending`), `GET/DELETE /api/logs`, `GET /api/storage`, `POST /api/storage/format`, `config/export|backup|restore`. Cihaz: `abort()` ile panik → coredump var; indir ve `espcoredump.py` ile çöz; fabrika 1/2; export/restore. Commit `feat(device): coredump, fabrika kademeleri, günlük ve depolama uçları`.
- [ ] **6.3 `api_remote.c` + uzaktan erteleme**: `GET /api/reset?key=` (26 kr base32, sabit zamanlı, IP tablosu 5/dk → 1 dk), `POST /api/remote/key`, `PUT /api/config/remote`; evlog. Cihaz: curl ile ok/401/429. Commit `feat(api): uzaktan erteleme anahtarı`.

### Adım 7 — Denetim

- [ ] **7.1 API sözleşme betiği tam**: `test_api.py` her uç (başarı/400/401/413/CSRF/maske/hız sınırı/404/405), AP üzerinden koşar; rapor. Commit `test(api): sözleşme betiği`.
- [ ] **7.2 Uçtan uca senaryolar + ölçümler** (`05-denetim.md`): sayaç/uyarı/tetik/teslimat/yeniden deneme; üç erteleme yolu; tatil; OTA tur + abort + rollback; fabrika 1/2; WiFi birincil→yedek→AP, 10 dk AP, 30 dk birincil; depolama hata/yokluk; brownout açılışı (K5) WDT ile simüle; tezgâh ölçümleri (tarama/dwell, AP_STOP/START, WPA3 reason, DNS/TCP/TLS süreleri, heap_min, hostname); kaynak raporu (imaj, GUI gz, yığın su izleri, heap). Her madde kanıt satırıyla. Commit `docs(2611): denetim raporu`.
- [ ] **7.3 Kapanış**: `yapilacaklar.md` güncelle, `README.md` (2611), hafıza notu, `00-surec.md` tamam.

---

## 4. Görev protokolü (her görev için)

1. `04-plan/adim-N.md`'deki görev bloğu alt ajana **tek başına** verilir: dosyalar, arayüz (bu belgeden kopya), test kodu, cihaz komutları, commit mesajı.
2. Sıra: başarısız test yaz → koş (FAIL) → en küçük uygulama → koş (PASS) → derle (`./esp_bin/build.sh`) → gerekiyorsa flash + log → commit.
3. İki aşamalı inceleme: (a) spec uyumu (arayüz imzaları, kısıtlar), (b) kod kalitesi (`/code-review` düzeyi medium). Düzeltme → yeniden inceleme → sonraki görev.
4. Adım sonunda: tüm host testleri, cihaz flash, açılış logu `esp_bin/logs/2611-adim-N.log`, `00-surec.md` durum.

## 5. Öz-denetim (yapıldı)

- **Kapsam:** 02-plan §3–6'nın her maddesi bir göreve bağlı; 03-arastirma §2 düzeltmeleri (14) ve §3 iyileştirmeleri görevlere işlendi; K1–K5 ve depo adı işlendi.
- **Yer tutucu:** yok; her görevin dosyası, testi, cihaz denetimi ve commit mesajı var; tam kod adım planlarında.
- **Tutarlılık:** `channel_result_t` = `CR_*` (outbox `result` alanı bunu taşır); `rt_blob_t` tek tanım (`core/include/outbox.h` içinde, `timer_sm` ve `outbox` paylaşır); `storage` mount adları `STG_LOG/STG_DATA`; TX gücü her yerde q-değeri (0,25 dBm); `job_t.seq` ↔ `EV_DELIVERY_RESULT.job_seq`.
