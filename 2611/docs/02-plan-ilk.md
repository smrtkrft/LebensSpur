# 02 — İlk plan: 2611 tasarımı

Tarih: 2026-11-05. Durum: kullanıcıyla bölüm bölüm onaylandı (A–D). Araştırma adımından (03) sonra `04-plan-son.md` olarak kesinleşir.

Girdi: `01-harita/01…07` (ALTE haritası, bulgu kaydı, ilkeler), `00-surec.md` karar günlüğü.

## 0. Amaç ve kapsam

ALTE/HW + ALTE/GUI'nin yaptığı her işi yapan, yeni özellik eklemeyen, ama yapısı baştan kurulmuş bir firmware + web arayüzü. Program ve arayüz yalnız dahili flash'ta; harici flash yalnız veri.

Kapsam dışı (bkz. `2611/yapilacaklar.md`): alarm sayısı/aralığı yeniden tasarımı, captive portal, şablon kütüphanesi, çoklu cihaz, SNTP sunucu ayarı, Secure Boot / Flash Encryption / NVS şifreleme (üretim adımı).

## 1. Ürün kararları (kullanıcı, 2026-11-05)

| # | Karar |
|---|---|
| 1 | Kapalı süre sayaçtan **düşmez**; sayaç yalnız cihaz açıkken ilerler (ALTE davranışı). |
| 2 | Tetikleme + teslimat sonrası sayaç **durur** (`TRIGGERED`); erteleme yeni tur başlatır. |
| 3 | **Her erteleme** (düğme / web / API anahtarı) röleyi söndürür. |
| 4 | `TRIGGERED`'da erteleme, kuyrukta **henüz başlamamış** tetikleme mesajlarını iptal eder; gönderimi süren tamamlanır. |
| 5 | Alarm sayısı/aralığı mantığı ALTE'deki gibi taşınır, tek dosyada izole edilir; kırpılmış değer API'de görünür. Yeniden tasarım ayrı oturum. |
| 6 | Telegram **yalnız erken uyarıda**. |
| 7 | SMTP sertifika doğrulaması **varsayılan açık**; kullanıcı kapatabilir (uyarılı, günlüklü). |
| 8 | Yapılandırılıp testi geçen kanal **otomatik açılır**; röle tetiklemede açık gelir. |
| 9 | Ek okunamazsa metin yine gider; eksik ek alıcıya, günlüğe ve GUI'ye bildirilir. |
| 10 | **Temiz kurulum**; ALTE NVS'den taşıma yok (ilk açılışta eski NVS silinir, günlüğe düşer). |
| 11 | Kurulum AP'si **parolasız**; GUI'den parola konabilir; tarayıcı parolası yokken GUI'de kalıcı uyarı. |
| 12 | Fabrika sıfırlama **iki kademe**: (1) ayarlar (ilk kurulum hali); (2) + harici flash biçimlenir. Fiziksel 5× basma = kademe 1; kademe 2 yalnız GUI'den, onaylı. |
| 13 | `/api/reset?key=` anahtarı 128 bit base32 (26 kr) + hız sınırı; uç ve parametre adı korunur. |
| 14 | Mesaj sınırları: konu 128, gövde 2048, alıcı listesi 512 bayt. |
| 15 | Yapılandırma: asıl kopya dahili NVS (64 KB); JSON yedeği harici flash'ta, **sırlar dahil**; NVS boşsa açılışta geri yükleme teklifi. |
| 16 | AP kapalıyken STA 10 dk bağlanamazsa AP **otomatik açılır**; STA dönünce kullanıcı ayarına döner. |
| 17 | Yedek ağdayken **30 dk'da bir birincil denenir**. |
| 18 | Mimari: iki katman, üç görev (`app_core`, `delivery`, `sys_worker`). |
| 19 | GUI: vanilla JS + derleme adımı (esbuild). |
| 20 | `./esp_bin/build.sh` varsayılan 2611; `--alte` eski ağaç. |
| 21 | Ek yükleme: `PUT /api/groups/<i>/files/<ad>` ham gövde. |
| 22 | Dahili flash sonundaki 128 KB `coredump` bölümü (teşhis). |

## 2. Mimari ilkeler

1. Olay güdümlü tek uygulama görevi; callback'ler yalnız olay bırakır, I/O yapmaz.
2. Teslimat katmanı (outbox): kalıcı, seri, yeniden denemeli; dört kanal tek arayüz.
3. Tek erteleme kapısı; her kaynak yetkili ve günlüklü.
4. Tek yapılandırma şeması; NVS, JSON, API, GUI formu, doğrulama aynı tablodan.
5. API tek zarf, tek ön işleyici; uzun iş = görev + durum ucu.
6. Güvenlik varsayılan açık (CSRF, oturum, hız sınırı, https OTA, sürüm düşürme reddi); AP açık (karar 11) ama görünür uyarı.
7. Depolama ayrımı: dahili = program + NVS; harici = `/data` (günlük, yedek, ekler, dosyalar), isteğe bağlı ve yokluğu görünür.
8. Tek sürüm kaynağı `PROJECT_VER`.
9. Güç kısıtı tasarım girdisi: az byte, az eşzamanlı TX, polling yok, büyük parçalı akış, seri gönderim.
10. Boyut: imaj ≤1,5 MiB, GUI ≤80 KB gz; ALTE'nin bilerek koruduğu teşhis özellikleri (WPA3, hata metinleri, `esp_err_to_name`, tam printf) değişmez.
11. Günlük tam ve dürüst: her silme/biçimleme/yeniden başlatma nedeni kayıtlı; kademe-1 sıfırlama günlüğü korur.
12. `platform/` `app/`'i bilmez; `app/` yalnız `platform/` başlıklarını görür.

## 3. Yapı ve katmanlar (A)

### 3.1 Dizin düzeni

```
2611/
  HW/
    CMakeLists.txt            PROJECT_VER — sürümün tek kaynağı
    partitions.csv
    sdkconfig.defaults
    main/main.c               açılış sırası ve görev kurulumu (~150 satır)
    components/
      platform/
        cfg/          şema tablosu, NVS blob, JSON dışa/içe, /data yedeği, fabrika sıfırlama
        storage/      harici SPI flash + LittleFS /data, dizinler, sağlık, biçimleme politikası
        evlog/        RTC kaydı + /data/log JSONL, reset nedeni, sys_restart()
        net_wifi/     WiFi durum makinesi, TX gücü, SNTP, mDNS, hostname
        net_tls/      tek esp-tls yapılandırması, zaman aşımları, CA paketi
        ota/          esp_https_ota, semver, koşullu onay, iptal, rollback
        http/         httpd, auth, CSRF, zarf, SSE, statik, rota tablosu
        sys_worker/   OTA ve depolama iş kuyruğu (görev)
      app/
        core/         app_core görevi, olay kuyruğu, timer_sm, relay_sm, warn_sched, checkin, status
        delivery/     outbox, delivery görevi, yeniden deneme politikası
        channels/     smtp, telegram, webhook, mqtt — channel_send()
        io/           düğme (ISR+debounce → olay), röle GPIO
        api/          HTTP rota işleyicileri (JSON ↔ cfg / durum / komut)
    test/host/                Unity + CMake masaüstü birim testleri
  GUI/
    src/                      index.html, css/, js/{api,i18n,state,ui/,pages/}, lang/, icons/
    build.py                  esbuild + minify + i18n doğrulama + sprite + sürüm → gz
  docs/
  yapilacaklar.md
esp_bin/2611/                 sarmalayıcı CMake, build/, sdkconfig (üretilen)
```

### 3.2 Görevler

| Görev | Öncelik | Yığın | Yapar | Yapmaz |
|---|---|---|---|---|
| `app_core` | 8 | 6 KB | olay kuyruğu; sayaç/röle/erteleme/tetikleme durum makineleri; outbox kararları; durum anlık görüntüsü | I/O (istisna: `rt` NVS blob'u, 60 s'de bir / geçişte) |
| `delivery` | 5 | 10 KB | outbox'tan tek tek iş alır, `channel_send()`, sonucu olay olarak döner | iki kanalı aynı anda |
| `sys_worker` | 3 | 8 KB | OTA, cfg yedeği, günlük boşaltma, ek dosya işleri, depolama sağlık | ağ mesajı |
| IDF (`esp_timer`, `wifi`, `tcpip`, `event`, `httpd`) | IDF | IDF | sürücü, ağ yığını, HTTP ayrıştırma; **callback'ler yalnız olay bırakır** | |

Kuyruklar: `app_core` 32 olay, `delivery` 16 iş, `sys_worker` 8 iş; dolu kuyruk → evlog hata (sessiz düşürme yok). Çalışma anında görev yaratılmaz.

### 3.3 Olay akışı (süre doldu)

```
esp_timer 1 Hz ─EV_TICK─▶ app_core: timer_sm → TRIGGERED
                           ├─ relay_sm TRIGGER (cfg: delay/pulse/duration) → io_relay (GPIO)
                           ├─ rt blob: state=TRIGGERED, trigger_pending[] (kalıcı)
                           ├─ outbox: grup mailleri (+ekler), webhook, mqtt → delivery
                           ├─ evlog "timer_expired" → sys_worker (dosya)
                           └─ status snapshot → SSE
delivery ─EV_DELIVERY_RESULT─▶ app_core: outbox güncelle, evlog, gerekirse yeniden sıraya
```

### 3.4 Açılış sırası

1. NVS init (bozuksa sil, evlog'a düşür, devam)
2. `cfg_load` → **röle güvenli seviyesi hemen** (inverted dahil) → düğme ISR
3. `evlog_init` (RTC'den önceki kapanış nedeni)
4. `storage_init` (harici flash; başarısızsa `ABSENT/ERROR`, devam)
5. `app_core` (rt blob'dan sayaç/tetikleme durumu) → `delivery` → `sys_worker`
6. `net_wifi` → IP/AP olayında `http`
7. OTA onayı: `http ayakta && (STA bağlı || 120 s)`; değilse bootloader geri döner

### 3.5 Dahili / harici ayrımı

Dahili 4 MB: bootloader, bölüm tablosu, NVS (ayarlar + rt), otadata, phy, ota_0/ota_1 (firmware + gömülü gzip GUI), coredump.
Harici `/data`: `log/`, `config/`, `mail/g<N>/`, `files/` (ayrılmış, 2611'de arayüzü yok). Harici flash yoksa cihaz çalışır: günlük yalnız RTC/RAM, ekler ve yedek kapalı, GUI rozet.

## 4. Uygulama çekirdeği (B)

### 4.1 `timer_sm`

| Durum | Giriş | Çıkış |
|---|---|---|
| `IDLE` (`total=0`) | fabrika | `set` |
| `RUNNING` | `set`, `checkin`, tatil bitişi, `cancel_vacation` | 0 → `TRIGGERED`; `cancel` → `STOPPED`; `vacation` |
| `VACATION` | `vacation(gün)` | tatil 0 → `RUNNING` (tam süre); `cancel_vacation` → `RUNNING` (kaldığı yerden) |
| `STOPPED` | `cancel` | `checkin`, `set` |
| `TRIGGERED` | 0'a inme | `checkin` → `RUNNING` (yeni tur); `set` |

- `EV_TICK` 1 Hz; kapalı süre sayılmaz.
- Erken uyarı: `kalan ≤ total·(n−k)/(n+1)`; açılışta geçmiş eşikler ateşlenmeden atlanır; tek tikte en fazla bir uyarı. `warn_sched.c`: `warn_threshold(total, n, k)`, `warn_resync(total, remaining, n)`, `warn_clamp(unit, value, n)` (ALTE `value/2`). Yeniden tasarım ayrı oturum.
- Sınırlar: süre 1..60 × {dk, saat, gün}; tatil 1..60 gün; uyarı 0..10.

### 4.2 Kalıcılık: NVS `rt` blob

`{schema, state, total_s, remaining_s, warn_fired, vac_total_s, vac_remaining_s, trigger_pending[10×4 bit], seq}`; her geçişte + `RUNNING`'de 60 s'de bir. Açılışta `TRIGGERED` + bekleyen → outbox yeniden kurulur.

### 4.3 `checkin(source)` — tek kapı

`source ∈ {BUTTON, WEB, API_KEY}`. (1) `total=0` → `no_timer`; (2) `VACATION` → sayaç etkilenmez, günlük, `ok`+not; (3) aksi → `remaining=total`, `warn_fired=0`, `RUNNING`; (4) röle **her durumda** söner; (5) `TRIGGERED` idiyse başlamamış tetikleme işleri iptal + günlük; (6) evlog `checkin(source)`, SSE.

### 4.4 `relay_sm`

`OFF → DELAY → ON | PULSE_ON ⇄ PULSE_OFF → OFF`; `cfg {inverted, delay_s 0..60, pulse_s 0..60, duration_s 0..3600 (0 = sınırsız)}`. `EV_TICK` ile 1 s çözünürlük; görev/`vTaskDelay` yok; yeni tetik eskisini anında sıfırlar; test her zaman süreli (0 → 5 s); açılışta güvenli seviye cfg okunur okunmaz.

### 4.5 Düğme

ISR + 200 ms debounce → `EV_BUTTON`; 400 ms pencere (`EV_BUTTON_WINDOW`) ile sayım: 1–2× erteleme, 3–4× yeniden başlat (`sys_restart(RST_USER_BUTTON)`), 5× fabrika kademe 1.

### 4.6 Outbox ve teslimat

- İş: `{id, olay EARLY_WARN(k)|TRIGGER|TEST, kanal SMTP|TELEGRAM|WEBHOOK|MQTT, hedef, deneme, sonraki, durum}`.
- Öncelik `TRIGGER > EARLY_WARN > TEST`; bir anda tek iş; ağ yoksa `EV_NET_UP` beklenir (deneme harcanmaz).
- Yeniden deneme: `TRIGGER` 1 → 5 → 15 dk → 30 dk'da bir, 24 saat; `EARLY_WARN` 2 deneme 1 dk ara; `TEST` yok.
- Zaman aşımları: bağlantı 10 s, el sıkışma 15 s, gönderim 120 s + ek başına 60 s; aşım → `FAILED_RETRY`, sıradaki alıcı.
- `EV_DELIVERY_RESULT` → outbox, evlog, SSE özeti ("3/5 gönderildi, 1 yeniden denenecek").

### 4.7 Kanallar

`esp_err_t channel_send(const job_t*, const cfg_t*, char err[96])`.
- **SMTP**: 465 doğrudan TLS, diğer portlar STARTTLS (ALTE sözleşmesi); her adımda yanıt kodu (4xx yeniden, 5xx kalıcı); tek oturum kodu; ekler `/data/mail/g<N>/` içinden 4 KB parça base64; eksik ek → gövdeye not + günlük + GUI; `tls_verify` cfg; sırlar logda asla.
- **Telegram**: yalnız `EARLY_WARN`; Bot API `sendMessage`.
- **Webhook**: yöntem GET/POST/PUT, JSON başlıklar, gövde şablonu `{status}{time}{device}`.
- **MQTT**: `mqtt://` ve `mqtts://` (CA paketi), QoS 1, tek yayın, bağlan-gönder-kapat.
- Etkinlik: `cfg.actions.early.{mail,telegram,webhook,mqtt}`, `cfg.actions.trigger.{mail,webhook,mqtt,relay}`.

### 4.8 Durum ve sınanabilirlik

`app_status_t` kısa mutex altında güncellenir; HTTP kopyasını okur. `timer_sm`, `relay_sm`, `warn_sched`, `checkin`, outbox politikası IDF bağımsız saf C → masaüstü Unity testleri.

## 5. Platform (C)

### 5.1 Yapılandırma (`cfg`)

Tek `cfg_t`; alan tablosu `{bölüm, ad, tip, offset, sınır, varsayılan, gizli}`; NVS'e bölüm başına blob:

| Blob | Alanlar |
|---|---|
| `device` | `ap_enabled`(1), `ap_password`("", gizli) |
| `wifi_p`, `wifi_b` | `ssid`(32), `pass`(64, gizli), `ip/gw/mask/dns`(15); boş `ip` = DHCP |
| `wifi_tx` | `max_dbm`(17), `adaptive`(1) |
| `timer` | `unit`, `value`(1..60), `warn_count`(0..10) |
| `relay` | `inverted`, `delay_s`, `pulse_s`, `duration_s` |
| `smtp` | `server`(63), `port`(465), `user`(63), `pass`(63, gizli), `tls_verify`(1) |
| `telegram` | `token`(71, gizli), `chat_id`(31), `message`(512) |
| `webhook` | `url`(255), `method`, `headers`(255), `body`(511) |
| `mqtt` | `url`(127), `topic`(127), `user`(63), `pass`(63, gizli), `payload`(255) |
| `early_mail` | `email`(63), `subject`(128), `body`(2048) |
| `grp0..grp9` | `used`, `name`(63), `subject`(128), `body`(2048), `recipients`(512) |
| `actions` | `early.{mail,telegram,webhook,mqtt}`, `trigger.{mail,webhook,mqtt,relay}` |
| `remote` | `enabled`, `key`(26, gizli) |
| `auth` | `hash`(64), `ttl_min`(10), `protect`(1), `lock_min`(15) |
| `meta` | `schema_version`, `seq` |

- `cfg_set_section(bölüm, cJSON*)` → doğrula → tek `nvs_set_blob` + commit → `sys_worker` "yedek yaz" → `EV_CFG_CHANGED(bölüm)`.
- Gizli alanlar API'de `"***"`; boş gelirse eski korunur; logda asla.
- Yedek `/data/config/config.json` + `config.prev.json` (`seq`, `schema_version`, sırlar dahil). Açılışta `meta` yok + yedek var → GUI "geri yükle?" (kullanıcı onayı).
- Fabrika kademe 1: cfg + rt silinir, meta yeniden; kademe 2: + `/data` biçimlenir. İkisi de evlog'a.

### 5.2 Depolama (`storage`)

- SPI (CS 21 / MISO 0 / MOSI 22 / SCLK 16), boyut çipten; tek LittleFS `/data` (offset 0, boyut = çip).
- Mount başarısız: bölge boşsa (ilk 4 KB `0xFF`) biçimle + günlük; doluysa **biçimleme**, `ERROR`, GUI rozet, kullanıcı biçimler.
- Ek sınırları: dosya ≤2 MB, grup ≤5 dosya, ≤4 MB; GUI'de gösterilir.
- Dosya işleri `sys_worker`'da; `delivery` yalnız okur.

### 5.3 Olay günlüğü (`evlog`)

- RTC kaydı ve kapanış nedeni sınıflandırması ALTE ile aynı (brownout/pwr_glitch/power_loss/poweron_rst/panic/*_wdt/user_restart/button_restart/factory_reset/ota/…; kısa açılış kuralı).
- `evlog_write()` bloklamaz: RAM halkası (32) + `sys_worker` boşaltma. Her görevden güvenli.
- `/data/log/events.jsonl`, 256 KB'ta `rename` ile döndürme; biçim ALTE JSONL (`type/n/ts/up/lvl/cat/msg`, boot kaydı alanları).
- Okuma sayfalı (`before`, `limit`), sondan.
- `sys_restart(reason)` tek yardımcı (OTA, USER_WEB, USER_BUTTON, FACTORY).

### 5.4 WiFi (`net_wifi`)

IDF olay döngüsünde tek durum makinesi; görev/`vTaskDelay`/bloklayan tarama yok (async scan).

```
AP_ONLY ─kayıt var─▶ SCANNING ─bulundu─▶ CONNECTING ─GOT_IP─▶ CONNECTED
   ▲                    ▲                    │ 5 deneme (1..5 s)     │ kopma
   └─ kayıt yok ─── RETRY_WAIT (15 s) ◀── EXHAUSTED (yedek → 5 daha) ◀┘
```

Korunan: AP SSID = cihaz adı, 192.168.4.1, 4 istemci, APSTA; birincil → yedek → gizli-kör sırası; reason 2/8 yeniden deneme yok; statik IP türetme; boş parola + aynı SSID = koru; mDNS `ls-….local`; SNTP yalnız STA; TX gücü tasarımı tamamen; modem uyku kapalı; AP STA bağlı değilken kapatılamaz.
Değişen: son hata nedeni durumda/API'de; `STA_LOST_IP`; kopmada IP sıfır; hostname; panik yerine evlog + AP'de kal; nicemlenmiş TX; `ESP_WIFI_NVS_ENABLED=n`; **AP kapalıyken 10 dk bağlanamazsa AP açılır** (karar 16); **yedekteyken 30 dk'da bir birincil** (karar 17).

### 5.5 TLS (`net_tls`)

`net_tls_cfg()`: CA paketi daima, bağlantı 10 s / el sıkışma 15 s; `insecure` yalnız SMTP ve yalnız kullanıcı kapattıysa. MQTT'de `crt_bundle_attach`.

### 5.6 OTA (`ota`)

`sys_worker`'da `esp_https_ota`; yalnız https; GitHub Releases → semver, yalnız daha yeni; özel URL → imaj `esp_app_desc` sürümü, daha eski ise red (`allow_downgrade` yoksa); tetiklemede `abort`; onay koşullu; rollback bilgisi doğru; `sys_restart(RST_OTA)`; coredump bölümü GUI'den okunur/silinir.

### 5.7 Güvenlik özeti

AP açık + GUI uyarısı · SHA-256(MAC:parola), `HttpOnly; SameSite=Strict`, çerez istek bağlamında, kilit IP başına · CSRF: `Origin/Host` **veya** `X-LS-Request: 1` · `/api/reset` 26 kr base32, sabit zamanlı, 5 hata/dk → 1 dk · OTA https + semver · sırlar maskeli/logsuz · gövde 413, JSON 400, dosya adı beyaz liste.

### 5.8 Bölüm tablosu ve sdkconfig

```
nvs       0x9000   0x10000
otadata   0x19000  0x2000
phy_init  0x1B000  0x1000
ota_0     0x20000  0x1E0000
ota_1     0x200000 0x1E0000
coredump  0x3E0000 0x20000
```
`sdkconfig.defaults`: ALTE'nin tüm boyut/teşhis kararları + `ESP_WIFI_NVS_ENABLED=n`, `ESP_COREDUMP_ENABLE_TO_FLASH=y`, httpd async.

## 6. API, arayüz, derleme, sınama (D)

### 6.1 HTTP katmanı

- Rota tablosu `{yol, yöntem, auth, csrf, max_body, handler}`; ön işleyici: oturum → CSRF → gövde sınırı (413) → cJSON (400) → handler.
- Zarf: `{"ok":true,"data":{…}}` / `{"ok":false,"error":{"code","field?","message"}}`; kodlar `auth_required, locked, bad_request, validation, not_found, busy, storage_absent, no_timer, internal`; HTTP kodları anlamlı.
- SSE `/api/events` async handler + `httpd_queue_work`; 60 s kalp atışı; mutex'li istemci listesi; oturumsuz 401 ile kapanır.
- Statik: hash'li adlar `immutable`; `index.html` `no-cache`; `favicon.svg`.

### 6.2 Uçlar

| Alan | Uçlar |
|---|---|
| Kimlik | `GET /api/auth` · `POST /api/auth/login` · `POST /api/auth/logout` · `PUT /api/auth/password` · `PUT /api/config/auth` |
| Durum | `GET /api/status` · `GET /api/events` (SSE) |
| Sayaç | `PUT /api/timer` · `POST /api/timer/checkin` · `POST /api/timer/cancel` · `POST /api/timer/vacation {days}` · `DELETE /api/timer/vacation` |
| Yapılandırma | `GET/PUT /api/config/<bölüm>` · `GET /api/config/export` · `POST /api/config/import` · `GET /api/config/backup` · `POST /api/config/restore` |
| Gruplar | `GET /api/groups` · `PUT/DELETE /api/groups/<i>` · `GET /api/groups/<i>/files` · `PUT/DELETE /api/groups/<i>/files/<ad>` |
| WiFi | `GET /api/wifi` · `POST /api/wifi/connect` · `DELETE /api/wifi/<p\|b>` |
| Testler | `POST /api/test/<smtp\|telegram\|webhook\|mqtt\|relay>` → `{job_id}` · `GET /api/jobs/<id>` |
| Uzaktan | `GET /api/reset?key=` (korunur) · `POST /api/remote/key` |
| Cihaz | `GET /api/device` · `POST /api/device/restart` · `POST /api/device/factory-reset {level}` · `GET /api/logs?before&limit` · `DELETE /api/logs` · `GET /api/storage` · `POST /api/storage/format` · `GET/DELETE /api/coredump` |
| OTA | `POST /api/ota/check` · `GET /api/ota` · `POST /api/ota/start` · `POST /api/ota/abort` · `POST /api/ota/rollback` |

### 6.3 Web arayüzü

- Modüler vanilla JS (`api, i18n, state, ui/, pages/`); ekranlar ALTE ile aynı: ana ekran + Timer / Connections / Actions / System / Info.
- Tek kaydetme modeli: form = bölüm = tek `PUT`; sunucudan dolmadan düzenlenemez; yalnız değişen form; "İptal" yeniden yükler; kirli formda sekme uyarısı; WiFi "Bağlan" ve "Durdur" onaylı; röle ayarları yüklenir.
- Trafik: SSE varken polling yok; yerel 1 Hz tik; SSE yoksa 30 s `status`; arka planda SSE kapalı; giriş ekranında sıfır istek; iş sorgusu 2 s.
- i18n: HTML'de metin yok; `lang/keys.json` + `lang/<xx>.json` değer dizisi; derlemede doğrulama; `replaceAll`; dil/tema tarayıcıda.
- Görsel: CSS değişkenleri, tema değişken seti, `!important` yok, SVG sprite, SVG logo, erişilebilirlik (aria, label, klavye, odak).
- Güvenlik: `textContent` varsayılan.
- Boyut (gz): HTML ≤11, JS ≤14, CSS ≤6, dil ×6 ≤33, sprite+logo ≤5 → ≤80 KB; aşımda derleme durur.
- Kalkanlar: GUI OTA/slot, "Çıkış → about:blank" (→ oturumu kapat), yol haritası notları.

### 6.4 Derleme ve sürüm

- `esp_bin/2611/CMakeLists.txt` sarmalayıcı; `./esp_bin/build.sh` varsayılan 2611, `--alte` eski.
- GUI: CMake configure → `python3 2611/GUI/build.py --ver ${PROJECT_VER} --out <build>/gui` → esbuild (npx) → i18n → sprite → sürüm/hash → gzip → `EMBED_FILES`.
- Sürüm yalnız `PROJECT_VER`.
- `-Wall -Wextra -Werror` proje bileşenlerinde.

### 6.5 Sınama ve denetim

| Katman | Ne | Nasıl |
|---|---|---|
| Masaüstü birim | timer_sm, relay_sm, warn_sched, checkin, outbox, cfg doğrulama/JSON, semver, reset sınıflandırıcı | Unity + CMake, `ctest` |
| Derleme | imaj ≤1,5 MiB, GUI ≤80 KB gz, i18n, `-Werror` | derleme durur |
| Cihaz, adım başı | build → flash → açılış logu → senaryo | `build.sh … flash`, pyserial |
| API sözleşmesi | her uç: başarı/400/401/413/CSRF/maske/hız sınırı | Python `requests`, Mac cihaz AP'sine bağlı |
| Uçtan uca | sayaç+uyarı+tetik+teslimat/yeniden deneme; üç erteleme yolu; tatil; OTA+onay+geri alma; fabrika 1/2; WiFi birincil→yedek→AP, 10 dk AP; depolama yokluğu | `05-denetim.md` kontrol listesi, her madde kanıtlı |
| Kaynaklar | görev yığın su izleri, heap min, imaj boyutu | `/api/device`, `idf.py size` |

## 7. Uygulama sıralaması (ön taslak; `04-plan-son.md`'de ayrıntılanır)

| Adım | İçerik | Doğrulama |
|---|---|---|
| 0 | İskelet: dizinler, CMake sarmalayıcı, `build.sh`, sdkconfig, bölüm tablosu, `main.c`, host test iskeleti | derle, flash, açılış logu, boş test geçer |
| 1 | `cfg` + `evlog` (RTC + `sys_restart`) + `storage` + `sys_worker` | host: cfg doğrulama/JSON; cihaz: `/data` mount, yedek yazımı, reset nedeni |
| 2 | `app/core`: timer_sm, relay_sm, warn_sched, checkin, düğme, rt kalıcılık | host testleri; cihaz: düğme/röle/kalıcılık |
| 3 | `net_wifi` + `net_tls` + `http` (auth, CSRF, zarf, status, events) + minimal GUI (ana ekran + Timer) | AP'den API ve GUI |
| 4 | `delivery` + 4 kanal + outbox kalıcılığı + test/iş uçları | gerçek SMTP/Telegram/webhook/MQTT testleri |
| 5 | GUI tamamı + i18n + boyut bütçesi | 6 dil, bütçe, erişilebilirlik |
| 6 | OTA + coredump + export/import/backup/restore + fabrika 1/2 + logs API | OTA tur, geri alma, sıfırlama kademeleri |
| 7 | Denetim: API betiği, uçtan uca senaryolar, kaynak raporu, `05-denetim.md` | tümü kanıtlı |

Her adım: yap → denetle → düzelt → yeniden denetle → commit → flash.

## 8. Araştırma soruları (adım 3)

1. **SSE**: IDF 5.5 `httpd_req_async_handler_begin` + `httpd_queue_work` ile uzun ömürlü soket; `max_open_sockets`, LRU purge etkileşimi.
2. **SMTP STARTTLS esp-tls ile**: mevcut TCP soketini TLS'e yükseltme (`esp_tls_plain_tcp_connect` / `esp_tls_set_conn_sockfd` / `esp_tls_set_conn_state`) destekleniyor mu? Değilse SMTP için tek, doğrulamalı mbedTLS sarmalayıcı.
3. **OTA sürüm denetimi GitHub API'siz**: `releases/latest/download/<ad>.bin` + `esp_https_ota_get_img_desc` ile sürümü okuyup devam/iptal — API kotası (60/saat) ve 4 KB JSON sorunu kalkar.
4. **esp_https_ota abort** ve yarım yazılmış bölümün durumu; `esp_https_ota_finish` olmadan iptal.
5. **NVS blob**: boyut/performans (2,8 KB blob'lar, 64 KB bölüm), `nvs_get_blob` kısmi okuma; ileride `nvs_sec_provider` HMAC.
6. **LittleFS (joltwallet)**: güç kaybı/rename atomikliği, biçim algılama (`esp_littlefs_mounted`, `format_if_mount_failed=false` + elle format), çip boyutundan bölüm tanımı.
7. **Coredump**: `esp_core_dump_image_check/get`, bölüm boyutu 128 KB yeterli mi (RISC-V, görev sayısı), API'den okuma.
8. **Async WiFi tarama** APSTA'da AP istemcilerine etkisi; `esp_wifi_set_max_tx_power` nicemleme tablosu; `esp_netif_set_hostname` zamanlaması.
9. **httpd istemci IP'si** (`httpd_req_to_sockfd` + `getpeername`) hız sınırı için; `CONFIG_HTTPD_MAX_REQ_HDR_LEN`.
10. **esbuild via npx** çevrimdışı/yeniden üretilebilir derleme (sabit sürüm, `package.json` + lock); SVG favicon desteği.
11. **Unity masaüstünde**: IDF `unity` bileşeni mi, bağımsız Unity mi; CMake/ctest düzeni.
12. **cJSON** bellek davranışı 2 KB gövdelerde; `cJSON_PrintPreallocated` ile yanıt üretimi.
13. **Telegram/webhook/MQTT** için esp_http_client/esp-mqtt zaman aşımı ve iptal; `esp_mqtt_client_destroy` süresi.
14. **Yedek dosyası sırları** için ileride NVS şifrelemesi açılınca yedek politikası (yapilacaklar).
15. Başka projelerde kanıtlanmış kalıplar: ESP-IDF "event-driven app core + outbox" örnekleri; dead-man's-switch tasarımlarında yeniden deneme politikaları.
