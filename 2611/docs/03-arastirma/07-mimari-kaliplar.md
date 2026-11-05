# 07 — Mimari kalıplar: olay güdümlü çekirdek, outbox, zaman, röle, bellek, şema, güç

Tarih: 2026-11-05. Girdi: `02-plan-ilk.md` (§2 ilkeler, §3.2 görevler, §3.3 olay akışı, §4 çekirdek, §4.6 outbox, §8 soru 15), `01-harita/07-bulgular-ve-oneriler.md` (C1, C4, C5, N2, N7, A1).

Kaynak gösterimi: `IDF:` = `/Users/seu/esp/esp-idf-v5.5.2/` (projenin derlendiği sürüm; satır numaraları bu ağaçtan). `ALTE:` = `/Users/seu/Code/SmartKraft/LebensSpur/ALTE/HW/`. Web kaynakları URL ile.

Her soru: **Bulgu** → **Öneri** ("plan aynen" / "şöyle değiştir") → **Risk**. Sonda "Plana etkisi".

## 0. Özet

| # | Konu | Karar |
|---|---|---|
| 1 | Olay güdümlü tek görev | **Plan aynen**; `esp_event` yalnız kaynak, kendi `xQueue`; `rt` blob yazımı `app_core`'da kalır ama dört kural altında |
| 2 | Outbox / teslimat | **Değiştir**: 24 sa → 72 sa (açık kalınan süre), %10 jitter, kalıcı/geçici hata ayrımı, sabit `Message-ID`, erken uyarı 3 deneme |
| 3 | Zaman ve sayaç | **Değiştir**: tik saymak yerine `esp_timer_get_time()` ile son tarih aritmetiği; `EV_TICK` yalnız "uyandır" |
| 4 | Röle ve bekçi | **Plan aynen** + donanım pull direnci şartı, `gpio_reset_pin` yasak, `delivery` için uygulama düzeyi süre bekçisi |
| 5 | Bellek | Tahmin: boşta ~300 KB, TLS+HTTP sırasında ~240 KB; **ölçüm adım 0'a**; dinamik mbedTLS tamponu **kapalı** |
| 6 | Şema tablosu | **Değiştir**: X-macro yerine küçük Python üreteci (`cfg_schema.json` → C + GUI JSON) |
| 7 | Güç | 9 somut kural; `esp_pm` kapalı kalır; brownout eşiği varsayılan; "brownout sonrası düşük güç açılışı" eklenir |

## 1. Olay güdümlü tek görev vs alternatifler

### 1.1 Bulgu

**Kalıp.** Samek'in "active object" tanımı: özel kuyruk + özel iş parçacığı + özel veri; olaylar yalnız bu iş parçacığında, kesintisiz (run-to-completion) işlenir; görev yalnız döngünün tepesinde, kuyruk boşken bloklanır; FreeRTOS üstünde `xQueueReceive` (bloklar) → `dispatch` (bloklamaz) iskeletiyle gösterilir (Samek, *Beyond the RTOS*, <https://www.state-machine.com/doc/Beyond-the-RTOS.pdf>; Embedded.com "Active objects in real-time", <https://www.embedded.com/?p=4481387>). Planın `app_core`'u birebir bu kalıp.

**`esp_event` kendi kuyruk yerine geçer mi?** Hayır:
- İşleyiciler döngü görevinde **sırayla** çalışır: `IDF:components/esp_event/esp_event.c:644` (`xQueueReceive`) → `:676/:686/:696` (`handler_execute`). Bir işleyici bloklarsa aynı döngüdeki tüm olaylar (varsayılan döngüde WiFi/IP olayları dahil) bekler.
- Varsayılan döngü önceliği 20 (`IDF:components/esp_system/include/esp_task.h:48`), yığın 2304 B, kuyruk 32 (`IDF:components/esp_system/Kconfig:213-221`). Uygulama mantığı burada çalışsaydı lwIP'nin (18) üstünde koşardı.
- Her `esp_event_post` olay verisini heap'e kopyalar (belge: "keeps a copy of event_data … allocation + deletion", <https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32c6/api-reference/system/esp_event.html>); kuyruk doluysa `ticks_to_wait` sonra `ESP_ERR_TIMEOUT` (`esp_event.c:955-976`); ISR'den gönderim yalnız 4 B veri (`:1016`).

**Başka projeler aynı yere varıyor.** ESPHome: tek iş parçacıklı bileşen döngüsü, `loop()` bloklayamaz, >10 ms gecikme yasak, çalışma anında "took a long time (…20–30 ms)" uyarısı (<https://developers.esphome.io/contributing/code-notes>). esp-rainmaker: olay işleyicilerinden işi `rmaker_work_queue` görevine (öncelik 5, yığın 4096) atar — planın `sys_worker`'ı ile aynı "erteleme kuyruğu" kalıbı (<https://docs.espressif.com/projects/esp-rainmaker/en/latest/esp32/c-api-reference/rainmaker_common.html>). Tasmota/WLED (Arduino `loop()`) da tek mantık döngüsü + sürücü görevleri.

**Öncelik sırası.** IDF yerleşik görevleri (`IDF:docs/en/api-guides/performance/speed.rst:201-211`): WiFi 23, `esp_timer` 22, olay döngüsü 20, lwIP 18, MQTT 5, `app_main` 1, FreeRTOS yazılım zamanlayıcı görevi 1. httpd varsayılanı `tskIDLE_PRIORITY+5`, yığın 4096 (`IDF:components/esp_http_server/include/esp_http_server.h:54-55`). Planın `app_core` 8 > `httpd` 5 = `delivery` 5 > `sys_worker` 3 sırası bütün sistem görevlerinin altında, kendi görevlerinin üstünde: doğru. Gerçek "öncelik tersine dönmesi" yalnız paylaşılan mutex'lerde olabilir: NVS küresel kilidi (`IDF:components/nvs_flash/src/nvs_api.cpp:66,100,130` — her API çağrısı tek `Lock`), durum anlık görüntüsü mutex'i, evlog halkası. FreeRTOS mutex'i öncelik mirası uygular; bekleme süresi kritik bölgenin uzunluğuyla sınırlı: en kötüsü httpd'nin 2,8 KB'lik `grp` blob'unu yazması.

**NVS yazımının gerçek maliyeti.**
- `nvs_commit` no-op: `IDF:components/nvs_flash/src/nvs_handle_simple.cpp:101-106`, `nvs_api.cpp:411-421` ("no-op for now"). Yani `nvs_set_blob` **anında** flash'a yazar; kalıcılık için commit beklemeye gerek yok (çağrı API sözleşmesi için kalır).
- Giriş 32 B, sayfa 126 giriş (`IDF:components/nvs_flash/private_include/nvs_constants.h:23-24`); tek parça en çok 4000 B (`nvs_page.hpp:43`). 64 B'lik `rt` blob'u ≈ 4 giriş (indeks + parça başlığı + 2 veri). Sayfa dolunca yeni sayfa + eski sayfanın silinmesi: 4 KB sektör silme.
- Flash süreleri (tipik SPI NOR veri sayfası, ör. W25Q32JV AC tablosu; modül içindeki çiple doğrulanmalı): sayfa programlama tipik 0,4 ms / en çok 3 ms; sektör silme tipik 45 ms / en çok 400 ms. IDF silmeyi 20 ms dilimlere böler ve aralarda 1 tik bırakır (`IDF:components/spi_flash/Kconfig:319-344`, `SPI_FLASH_YIELD_DURING_ERASE=y`).
- Dilim sırasında C6'da (tek çekirdek) **bütün görevler askıda** (`IDF:components/spi_flash/cache_utils.c:278` `vTaskSuspendAll`) ve IRAM dışı kesmeler kapalı (`:298` `esp_intr_noniram_disable`). Bu sistem genelidir: blob'u hangi görev yazarsa yazsın diğerleri aynı süre durur. Yazımı `sys_worker`'a taşımak yalnız `app_core`'un kendi gecikmesini kaldırır, sistemin donmasını değil.
- Auto-suspend C6'da donanımca var (`IDF:components/soc/esp32c6/include/soc/soc_caps.h:428-431`) ama belge Wi-Fi gibi sık kesme üreten senaryolar için "kullanma" diyor (`IDF:docs/en/api-reference/peripherals/spi_flash/auto_suspend.inc:40`); varsayılan kapalı (`spi_flash/Kconfig:86-94`).

**ISR ve timer callback'inden kuyruğa.** FreeRTOS fonksiyonları varsayılan olarak IRAM'de (`IDF:components/freertos/Kconfig:559-561`, `FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=n`); GPIO ISR `ESP_INTR_FLAG_IRAM` ile ayrılırsa (`IDF:components/esp_hw_support/include/esp_intr_alloc.h:42`) flash yazımı sırasında da çalışır → düğme basışı NVS yazımı sırasında kaybolmaz. ALTE zaten `xQueueSendFromISR` + `portYIELD_FROM_ISR` kullanıyor (`ALTE:components/output/output.c:55-70`). `esp_timer` callback'i öncelik 22, yığın 3584 B görevde koşar (`IDF:components/esp_timer/Kconfig:25-28`): yalnız `xQueueSend(q, &ev, 0)`.

### 1.2 Öneri — plan aynen, dört kural ekle

1. **Olay = sabit boyutlu POD, heap yok.** `typedef struct { uint8_t type; uint8_t src; uint16_t arg16; uint32_t arg32; } app_ev_t;` (8 B). Büyük veri (cfg bölümü, teslimat sonucu metni) olayla taşınmaz; olay "değişti" der, `app_core` kaynağından okur.
2. **Kuyruk dolu politikası** (sessiz düşürme yok, plan §3.2):

| Kaynak | Bekleme | Dolu olursa |
|---|---|---|
| `esp_timer` tik, GPIO ISR, WiFi olay işleyicisi | 0 | sayaç `q_drop++`; `app_core` bir sonraki turda `evlog` yazar (ISR'den log yok) |
| httpd işleyicisi (komut) | ≤100 ms | `503 busy` zarfı (plan kodu `busy`) |
| `delivery`, `sys_worker` sonucu | ≤1 s | sonucu kendi durumunda tutar, `EV_*_RESULT` tekrar dener (kaybolmaz) |

   Tik düşerse sayaç etkilenmez (bkz. §3: kalan süre tik sayımından değil saatten türer).
3. **`rt` blob yazımı `app_core`'da kalır, ama:** (a) yalnız durum geçişinde ve 60 s kontrol noktasında; (b) içerik değişmediyse yazılmaz (son yazılan kopyayla `memcmp`); (c) çağrı süresi `esp_timer_get_time()` ile ölçülür, >200 ms ise `evlog` uyarısı; (d) `app_core` başka hiçbir flash/dosya API'sine dokunmaz — `app/core` CMake `REQUIRES` listesinde `esp_littlefs`, `storage`, `esp_http_client` yok, derleme bunu zorlar. Hesap: 60 s'de bir 4 giriş → sayfa ~30 yazımda dolar → ~30 dk'da bir sektör silme (20 ms dilimli ≤ ~0,5 s en kötü). 64 KB NVS = 16 sayfa → her sayfa ~8 sa'de bir silinir → 100 k döngü ≈ 90 yıl; aşınma sorun değil.
   Seçenek (ölçüm >200 ms gösterirse): 60 s kontrol noktası `sys_worker`'a anlık kopyayla devredilir, **geçişler** (`TRIGGERED`, `trigger_pending` değişimi, `checkin`) her zaman senkron `app_core`'da kalır — bulgu C1'in çözümü bu senkronluktur.
4. **`esp_event` yalnız kaynak.** WiFi/IP işleyicisi: `app_core_post(EV_NET_UP/DOWN, reason)` ve döner; `esp_event` kuyruğunu asla beklemez.

İskelet (`app/core/app_core.c`):

```c
static QueueHandle_t s_q;                       /* 32 × app_ev_t, statik: xQueueCreateStatic */
static uint32_t s_q_drop;

bool app_core_post(app_ev_t ev)                 /* görevlerden: 0 bekleme */
{
    if (xQueueSend(s_q, &ev, 0) == pdTRUE) return true;
    __atomic_add_fetch(&s_q_drop, 1, __ATOMIC_RELAXED);
    return false;
}
bool IRAM_ATTR app_core_post_isr(app_ev_t ev)   /* ISR'den */
{
    BaseType_t hp = pdFALSE;
    bool ok = xQueueSendFromISR(s_q, &ev, &hp) == pdTRUE;
    if (!ok) s_q_drop++;
    if (hp) portYIELD_FROM_ISR();
    return ok;
}
static void app_core_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        app_ev_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int64_t t0 = esp_timer_get_time();
            core_dispatch(&ev);                 /* saf C durum makineleri; bloklamaz */
            if (esp_timer_get_time() - t0 > 200000) evlog_warn("core_slow", ev.type);
        }
        if (s_q_drop) { evlog_warn("q_drop", s_q_drop); s_q_drop = 0; }
        core_poll_deadlines();                  /* §3: sayaç/röle/erteleme süreleri */
        rt_persist_if_dirty();                  /* §1.2 kural 3 */
        esp_task_wdt_reset();                   /* §4 */
    }
}
```

### 1.3 Risk

- WiFi yeniden bağlanma fırtınası (STA kopma + AP istemci olayları) 32'lik kuyruğu anlık doldurabilir; düşen olaylar yalnız "bilgi" türünde olduğu için sayaç/röle etkilenmez, ama `q_drop` günlüğü görülmeli (denetim maddesi).
- `cfg_set_section` (httpd) ile `rt` yazımı NVS kilidinde çakışır; `app_core` en çok bir blob yazımı kadar bekler (≤ ~0,5 s). Sayaç saatten türediği için kayıp yok; röle kenarı en çok bu kadar gecikir (ürün için kabul edilebilir, §4).
- Kural 3(d) ihlali (bir geliştirici `app_core`'dan dosya yazar) → harici SPI flash mutex'i + LittleFS silme yüzlerce ms bloklar. CMake `REQUIRES` beyaz listesi ve kod incelemesi maddesi.

## 2. Outbox / teslimat garantisi

### 2.1 Bulgu

**Standartlar ve MTA'lar.** RFC 5321 §4.5.4.1: yeniden deneme aralığı en az 30 dk (SHOULD), vazgeçme süresi en az 4–5 gün (<https://www.rfc-editor.org/rfc/rfc5321#section-4.5.4>). Postfix varsayılanları: `minimal_backoff_time 300s`, `maximal_backoff_time 4000s`, `maximal_queue_lifetime 5d`, `queue_run_delay 300s` (postconf(5), <https://manpages.debian.org/bookworm/postfix/postconf.5.en.html>). Yani MTA dünyası: 5 dk'dan başlayıp ~67 dk'ya çıkan üstel aralık, 5 gün.

**Bulut IoT SDK'leri.** AWS: "exponential backoff and jitter" — Full Jitter: `sleep = rand(0, min(cap, base·2^n))`; sonuç: jitter yük tepesini düz dağıtır, "uzak istemciler için standart yaklaşım" (<https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/>). FreeRTOS `backoffAlgorithm` (coreMQTT/esp-aws-iot demoları) aynı Full Jitter'ı uygular; örnek değerler taban 500 ms, tavan 5 s, 5 deneme — bu **bağlantı** düzeyi yeniden deneme, mesaj düzeyi değil (<https://github.com/FreeRTOS/backoffAlgorithm>). Azure IoT SDK: varsayılan "exponential back-off with jitter"; hatalar **kurtarılabilir / kurtarılamaz** diye sınıflanır, kurtarılamazda (kimlik hatası, yanlış uç) deneme durur ve kullanıcıya bildirilir; "no retry" seçilirse mesaj kaybolur (<https://learn.microsoft.com/en-us/azure/iot-hub/concepts-manage-device-reconnections>).

**esp-mqtt outbox kalıcı değil.** RAM'de tutulur, yayımlanmadan 30 s bekleyen mesaj silinir (`IDF:components/mqtt/esp-mqtt/Kconfig:173-178`, `MQTT_OUTBOX_EXPIRED_TIMEOUT_MS=30000`). Planın "bağlan-gönder-kapat, QoS1, PUBACK bekle" yaklaşımı doğru; kalıcılık bizim outbox'ta olmalı.

**Dead man's switch ürünleri.** Alcazar: kaçırılan check-in'de önce tüm kanallardan hatırlatma, sonra eylem (<https://blog.alcazarsec.com/posts/what-is-a-dead-mans-switch>); MyGoodTrust: seçilen aralıkta e-posta, **3 yanıtsız** turdan sonra tetikleme (<https://support.mygoodtrust.com/support/solutions/articles/66000503696>); Healthchecks.io: "down" geçişinde her entegrasyona bir uyarı + isteğe bağlı saatlik/günlük "hâlâ down" hatırlatması (<https://healthchecks.io/docs>, yapılandırma belgesi). Ortak nokta: tetikleme **bir kez** bildirilir, ama bildirimin ulaşması için kanal başına yeniden deneme ve ikinci kanal (ör. e-posta spam'e düşerse başka kanal) önerilir. Planın çok kanallı, kanal başına bağımsız durum tutan outbox'ı bununla uyumlu.

**`Message-ID` ve tekilleşme.** Exchange/Office 365/Google Workspace sunucu tarafında aynı `Message-ID`'li ikinci kopyayı sessizce düşürür (Chilkat, <https://chilkatsoft.com/email_duplicate_message_id.asp>; Mailman listesi Exchange örneği, <https://www.python.org/pipermail/mailman-users/2013-November/075731.html>). Fastmail klasör başına aynı `Message-Id`'yi atar (<https://fastmail.com/blog/enable-duplicate-deliveries>); Exim posta kutusu düzeyinde aynı denetimi sunar. Tüketici Gmail/Outlook istemcileri için belgelenmiş garanti yok. Sonuç: sabit `Message-ID` **çoğu kurumsal** alıcıda tekilleştirir, diğerlerinde en kötü iki kopya.

### 2.2 Öneri — şöyle değiştir

**Politika tablosu (`app/delivery/retry_policy.c`, saf C, host testli):**

| Olay | Denemeler | Vazgeçme | Not |
|---|---|---|---|
| `TRIGGER` | 0 → 1 → 5 → 15 → 30 dk, sonra 30 dk'da bir; her bekleme ±%10 jitter | **72 sa açık kalınan süre** ya da erteleme (karar 4) | 24 sa yerine 72 sa: mesajın değeri 1. günden sonra düşmez; RFC 4–5 gün der; 30 dk'da bir TLS = günde 48 el sıkışma, önemsiz |
| `EARLY_WARN(k)` | 0 → 1 → 5 dk (3 deneme) | bir sonraki uyarı eşiği ya da tetikleme gelince iptal | greylisting (451, tipik 1–15 dk) ilk denemeyi reddeder; 2 deneme 1 dk ara yetmez |
| `TEST` | 1 | — | sonuç `GET /api/jobs/<id>` |

**Hata sınıfı** (Azure modeli): *kalıcı* → deneme durur, `FAILED_PERM`, GUI + evlog'da neden: SMTP 5xx (`421/450/451/452` hariç), Telegram 400/401/403, webhook 4xx (`408/429` hariç), MQTT CONNACK ret (kimlik). *Geçici* → tabloya göre: DNS, TCP bağlantı, TLS, zaman aşımı, SMTP 4xx, HTTP 5xx/429, ağ yok. Ağ yokken deneme harcanmaz (plan aynen); `EV_NET_UP` sonrası **10 s yerleşme** beklenir (DHCP/ARP, RSSI; §7 R7).

**Jitter gerekli mi?** Tek cihaz, sunucu yükü sorunu yok; ama 30 dk'lık deneme periyodu planın **30 dk'da bir birincil ağ denemesi** (karar 17) ile hizalanabilir — tam deneme anında WiFi geçişi. ±%10 jitter (3 dk) + ağ olayına bağlı erteleme bunu kırar. Ucuz; ekle.

**Kalıcı kayıt: bitmask + sayaç yeter, tam iş kaydı gereksiz.** `rt` blob'una: `trigger_seq` (u32), `since_trigger_s` (u32, açık kalınan süre; 60 s kontrol noktasıyla ilerler), `pending[grup 10 + webhook + mqtt]` (bit), `attempts[]` (nibble), `early_pending` (bit, k). Zaman damgası, son hata metni ve deneme zamanları RAM'de + evlog JSONL'de (zaten yazılıyor); yeniden başlatmada tablo "deneme 0, hemen" olarak kurulur — yeniden başlatma = taze ağ, hemen denemek doğru. 72 sa sınırı `since_trigger_s`'tan hesaplanır.

**Yarım kalan gönderim.** Belirsizlik penceresi yalnız "`CRLF.CRLF` gönderildi, `250` okunmadı" aralığı. Kural: *pending* biti **yalnız `DATA` sonrası `250` okununca** temizlenir ve `rt` blob'u hemen yazılır (alıcı grubu başına bir yazım). Güç kesilirse yeniden gönderilir; iki kopya kabul edilir — ölü adam anahtarında tehlikeli olan **göndermemek**tir. Tekrarı en aza indirmek için:
- SMTP: `Message-ID: <ls-{trigger_seq}-g{grup}@{hostname}.local>` — deneme sayısından bağımsız, aynı tetikleme turunda sabit. `Date` ilk denemenin saati (yoksa uptime'dan türetilmiş sabit).
- Webhook: gövdeye `"event_id": "{trigger_seq}-{olay}"`; başlığa `Idempotency-Key`.
- MQTT: QoS1 zaten en-az-bir; yüke `seq`.
- Telegram: tekilleştirme yok; erken uyarı olduğundan çift mesaj zararsız.

### 2.3 Risk

- 72 sa boyunca 30 dk'da bir TLS: heap parçalanması birikebilir; her denemede `largest_free_block` ölçülüp evlog'a düşer (§5).
- Kalıcı hata sınıflandırması yanlışsa (ör. SMTP 535 kimlik hatası geçici sayılırsa) 72 sa boşuna denenir; tersi (geçici hatayı kalıcı sayma) mesaj kaybıdır — tablo host testiyle sabitlenir, şüphede **geçici** sayılır.
- Sabit `Message-ID` ile ilk denemede ek eksik, ikinci denemede ek tam gönderilirse Exchange ikinciyi düşürür. Kural: ek eksikliği gönderim içeriğini değiştiriyorsa `Message-ID`'ye `-r{attempt}` eklenir (karar 9'daki "eksik ek notu" durumu).

## 3. Zaman ve sayaç

### 3.1 Bulgu

- C6'da `esp_timer` SYSTIMER üstünde (`IDF:components/esp_timer/Kconfig:120-123`), SYSTIMER kaynağı **XTAL 40 MHz** (`IDF:components/hal/esp32c6/include/hal/systimer_ll.h:29`, `IDF:components/soc/esp32c6/include/soc/clk_tree_defs.h:151`); CPU frekansından ve DFS'ten bağımsız. Kristal gereksinimi ±10 ppm (ESP32-C6 donanım tasarım kılavuzu, şematik denetim listesi: <https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c6/schematic-checklist.html>). 60 gün × 10 ppm = **±52 s** en kötü birikimli sapma; 60 s'lik kontrol noktası tanesinin altında.
- Periyodik `esp_timer` alarmı `alarm += period` ile ilerler (`IDF:components/esp_timer/src/esp_timer.c:420`) — callback gecikmesi **birikmez**; görev geç kalırsa kaçan periyotlar ya tek tek yetiştirilir ya da `skip_unhandled_events` ile atlanıp sayılır (`:414-417`). Callback'ler öncelik 22 görevden gelir, gecikebilir (`IDF:docs/en/api-reference/system/esp_timer.rst:165`). `esp_timer_get_time()` 1 µs, kilitsiz, ISR'de güvenli (`esp_timer.rst:92`).
- RTC yavaş saat C6 varsayılanı **136 kHz dahili RC** (`IDF:components/esp_hw_support/port/esp32c6/Kconfig.rtc:1-8`); yalnız uyku sırasında zaman tutar (`esp_timer.rst:152`). Uyku kullanılmadığı sürece sayaç için önemsiz; `esp_pm` light-sleep açılırsa sayaç bu kaba saate düşer (bir neden daha, §7).
- FreeRTOS tik 100 Hz varsayılan (`IDF:components/freertos/Kconfig:33-37`); 1000 Hz yalnız ms çözünürlüklü `vTaskDelay` için yarar, tik kesmesi yükü artar; IWDT tik periyodunun en az 2 katı olmalı (`IDF:docs/en/api-reference/system/wdts.rst:72`). FreeRTOS yazılım zamanlayıcı görevi öncelik 1 (`speed.rst:203`) → `xTimer` httpd'nin bile altında; uygun değil.
- Tik saymanın zayıflığı: tik düşerse (kuyruk dolu, §1.2) ya da `app_core` 0,5 s bloklanırsa sayaç geri kalır; 60 günde onbinlerce tik → küçük kayıplar birikir ve test edilemez.

### 3.2 Öneri — şöyle değiştir: son tarih aritmetiği

`timer_sm` kalan süreyi tik saymaz; `deadline_us` tutar ve her uyanışta `remaining = deadline − now` hesaplar. `EV_TICK` (1 Hz, `skip_unhandled_events = true`) yalnız "değerlendir" sinyalidir; birden çok tik gelse de sonuç aynı. Kalıcılıkta `remaining_s` yazılır (uptime yeniden başlar); açılışta `deadline = now + remaining_s·1e6`. Tatil, röle gecikme/darbe/süre, erken uyarı eşikleri, düğme 400 ms penceresi, erteleme hız sınırı — hepsi aynı `now` ile. Saat enjekte edilebilir (`core_clock_t`), masaüstü testinde sahte saat.

```c
typedef int64_t (*now_fn)(void);                 /* esp_timer_get_time ya da test saati */
typedef struct {
    tsm_state_t st; int64_t deadline_us; int64_t vac_deadline_us;
    uint32_t total_s; uint8_t warn_n, warn_fired;
} timer_sm_t;

void tsm_eval(timer_sm_t *t, int64_t now, tsm_out_t *out)   /* her EV_TICK ve her olayda */
{
    if (t->st == TSM_RUNNING) {
        int64_t rem = t->deadline_us - now;
        if (rem <= 0) { t->st = TSM_TRIGGERED; out->trigger = true; return; }
        uint8_t k = warn_index(t->total_s, (uint32_t)(rem / 1000000), t->warn_n);
        if (k > t->warn_fired) { t->warn_fired = k; out->early_warn = k; }   /* tikte en çok bir */
    } else if (t->st == TSM_VACATION && now >= t->vac_deadline_us) {
        t->st = TSM_RUNNING; t->deadline_us = now + (int64_t)t->total_s * 1000000;
    }
}
uint32_t tsm_remaining_s(const timer_sm_t *t, int64_t now)   /* rt blob + /api/status */
{
    int64_t rem = t->deadline_us - now; return rem > 0 ? (uint32_t)(rem / 1000000) : 0;
}
```

Tik hâlâ gerekli (SSE'ye saniyelik durum, röle kenarları). 100 Hz tik ve varsayılan `esp_timer` ayarları kalır. Duvar saati (SNTP) sayaçta kullanılmaz (karar 1); yalnız evlog damgası ve `Date` başlığı.

### 3.3 Risk

- `esp_timer_get_time()` yeniden başlatmada sıfırlanır; `deadline` asla NVS'e yazılmaz, yalnız `remaining_s` (kod incelemesi maddesi).
- Uyku modu açılırsa (ileride) `esp_timer` sayacı RTC'den ilerletilir; 136 kHz RC ile %5'e varan hata. Uyku açılacaksa `RTC_CLK_SRC_EXT_CRYS` donanımı gerekir; şimdi kapsam dışı, `yapilacaklar.md`'ye not.

## 4. Röle ve güvenlik

### 4.1 Bulgu

- **GPIO19** C6'da IO_MUX `SDIO_CLK` pad'i (`IDF:components/soc/esp32c6/register/soc/io_mux_reg.h:113`); F0 = SDIO_CLK, F1 = GPIO, F2 = FSPICS3 (`:254-257` kalıbı; ESP-AT SDIO kılavuzu da CLK=GPIO19 der, <https://docs.espressif.com/projects/esp-at/en/latest/esp32c6/Compile_and_Develop/How_to_implement_SDIO_AT.html>). Strapping pini değil (strapping: 4, 5, 8, 9, 15). ROM ve bootloader bu pine dokunmaz; `app_main`'e kadar (~150–300 ms) pad yüksek empedansta kalır. ALTE'de röle GPIO19 aktif-yüksek, düğme GPIO18 (`ALTE:components/output/output.c:21-22`).
- `gpio_reset_pin()` **pull-up'ı açar** (`IDF:components/esp_driver_gpio/include/driver/gpio.h:74`: "select gpio function, enable pullup and disable input and output"). Aktif-yüksek bir transistör sürücüsünde 45 kΩ pull-up tabanı hafifçe sürebilir; röle pininde çağrılmamalı.
- TWDT: varsayılan 5 s, yalnız uyarı; ALTE 10 s + `PANIC=y` (`ALTE:sdkconfig.defaults`; Kconfig `IDF:components/esp_system/Kconfig:500-516`). Idle görevi izlenir (`:518-521`); kullanıcı görevleri `esp_task_wdt_add` ile abone olur, `esp_task_wdt_reset` ile besler; alt-işlem düzeyi için `esp_task_wdt_add_user`; çalışma anında `esp_task_wdt_reconfigure` (`IDF:components/esp_system/include/esp_task_wdt.h:63,90,105`). Belge: zaman aşımı "bir görevin CPU'yu bırakmadan koşabileceği en uzun süre"den uzun olmalı (`wdts.rst:123`); uzun flash silmelerinde aşım uyarısı (`:127-129`). IWDT 300 ms (`esp_system/Kconfig:458-463`).
- `esp_https_ota_perform` parça parça döner (`ESP_ERR_HTTPS_OTA_IN_PROGRESS`, `IDF:components/esp_https_ota/include/esp_https_ota.h:153-163`) → `sys_worker` her parçada bekçiyi besleyebilir.
- TLS'de bloklayan yerler soket bekleyişleridir (lwIP `select`, görev bloklanır, idle koşar); CPU'yu tutan parçalar (sertifika zinciri doğrulama, ECDHE) C6'da yüzlerce ms — 10 s'nin çok altında. TWDT idle izlemesi `delivery`'yi zaten dolaylı kollar; kollamadığı şey **kilitlenme**dir (N2: `WANT_READ` döngüsü).

### 4.2 Öneri — plan aynen, şunlar eklenir

**Röle.**
1. Donanım: sürücü girişinde röle **enerjisiz** seviyesini tutan direnç (aktif-yüksek → 10 kΩ pull-down; `inverted` → pull-up). Yazılım açılıştan önceki pencereyi kapatamaz; bu tek güvenilir çözüm. GUI'de `inverted` seçeneğinin yanına "harici pull-up gerekir" notu.
2. Yazılım, `app_main` ilk satırları (NVS'ten **önce**): `gpio_set_level(RELAY, 0)` → `gpio_set_direction(RELAY, OUTPUT)` → `gpio_set_pull_mode(RELAY, FLOATING)`; `gpio_reset_pin` yok. `cfg_load` sonrası `inverted` ise seviye düzeltilir (bu pencere ~10–50 ms, donanım direnciyle örtülür).
3. 1 s çözünürlük yeter (darbe 1..60 s, süre 0..3600 s); kenarlar §3'teki son tarih aritmetiğiyle hesaplanır, `app_core` 0,5 s bloklansa bile sıra bozulmaz, en fazla o kadar gecikir. Daha ince çözünürlük ürün gereği değil.

**Bekçi.**

| Görev | TWDT | Besleme | Gerekçe |
|---|---|---|---|
| `app_core` | abone | her döngü turu; `xQueueReceive` zaman aşımı 1 s | kuyruk boşken de besler; 10 s'de beslenmezse gerçek kilitlenme |
| `delivery` | **abone değil** | — | 10–15 s soket zaman aşımları + 120 s gönderim TWDT penceresini aşar; parçalı besleme kodu kirletir |
| `delivery` (uygulama bekçisi) | `app_core` izler | iş başlangıcında `deadline = now + bağlantı 10 + el sıkışma 15 + gönderim 120 + 60×ek + 30 s pay` | aşımda: `shutdown(sock)` ile uyandır; 10 s içinde `EV_DELIVERY_RESULT` gelmezse `evlog` + `sys_restart(RST_DELIVERY_HANG)` |
| `sys_worker` | abone | OTA'da her `esp_https_ota_perform` dönüşünde; dosya işinde her 4 KB'ta | LittleFS silme/yazma sınırlı; OTA parçalı |
| idle | abone (varsayılan) | — | CPU'yu tutan hatalı döngüleri yakalar |

`CONFIG_ESP_TASK_WDT_TIMEOUT_S=10`, `PANIC=y` korunur; TWDT paniği evlog'da `task_wdt` olarak sınıflanır (ALTE davranışı).

### 4.3 Risk

- `delivery`'nin uygulama bekçisi yeniden başlatma yaparsa yarım gönderim §2.2 kuralıyla yeniden denenir; bekçi süresi gerçek süreden kısa ayarlanırsa gereksiz yeniden başlatma döngüsü. `since_trigger_s` ile 3 ardışık `RST_DELIVERY_HANG` görülürse zaman aşımları iki katına çıkarılır (evlog'a düşer).
- `inverted` + pull-up yokken her açılışta ~200 ms "tetiklenmiş" darbesi (bulgu C9) yazılımla **tam** çözülemez; belgelenir.

## 5. Bellek bütçesi

### 5.1 Bulgu

- C6 HP SRAM 512 KB tek bölge (`IDF:components/soc/esp32c6/include/soc/soc.h:162-165`, IRAM ve DRAM aynı aralık): IRAM'e yerleşen kod doğrudan heap'ten eksilir. `hello_world` boş heap ~468 KB (topluluk karşılaştırması, yaklaşık: <https://github.com/tyeth/esp32-lang-lab>).
- WiFi tamponları (varsayılan): statik RX 10 (`IDF:components/esp_wifi/Kconfig:26-30`), dinamik RX 32 (`:42-46`), dinamik TX 32 (`:118-122`); IRAM seçenekleri `ESP_WIFI_IRAM_OPT` (>10 KB), `RX_IRAM_OPT` (>17 KB), `EXTRA_IRAM_OPT` (>5 KB, C6'da varsayılan açık) (`:274-300`). Statik WiFi tamponları `esp_wifi_init`'te heap'ten alınır (`IDF:docs/en/api-guides/performance/ram-usage.rst:36,154`).
- mbedTLS: giriş 16384 / çıkış 4096 (`IDF:components/mbedtls/Kconfig:82-95`, asimetrik varsayılan açık). Dinamik tampon `MBEDTLS_DYNAMIC_BUFFER` (varsayılan kapalı; DTLS ve değişken tampon ile uyumsuz, `:100-105`), `DYNAMIC_FREE_CONFIG_DATA`, `DYNAMIC_FREE_CA_CERT` (`:117-133`). Ölçüm (Toit): dinamik tampon TLS+bignum 36 → 12 KB, peer sertifikası tutulmayınca ~8 KB (<https://help.toit.io/transcripts/1546501960814043227.html>). Bedel: her kayıtta 16 KB **bitişik** blok yeniden ayrılır → parçalanmada başarısızlık; geçmişte `esp_http_client` ile 16 B sızıntı (düzeltildi, <https://github.com/espressif/esp-idf/issues/6104>).
- IDF görev yığınları: `esp_timer` 3584 (`esp_timer/Kconfig:27`), olay 2304 (+2048 koşullu), lwIP 3072 (`IDF:components/lwip/Kconfig:872-874`), `main` 3584 (`esp_system/Kconfig:225-227`), httpd 4096 varsayılan (`esp_http_server.h:55`), WiFi görevi ~6,5 KB (kütüphane içi), idle ~1,5 KB → ~25 KB; bizim görevler 24 KB (plan).
- Belge uyarısı: toplam boş heap yanında **en büyük boş blok** izlenmeli (`ram-usage.rst:29`).
- ALTE `/api/system/detail` zaten `heap_total/heap_free/heap_min` döner (`ALTE:components/system_info/system_info.c:55-57`); cihazda ölçüm aracı hazır.

**Tahmin (ölçülene kadar):**

| Kalem | KB | Not |
|---|---|---|
| Açılış boş heap | ~468 | hello_world referansı |
| WiFi init+start (APSTA) | −65…75 | statik RX 16 KB + sürücü + AP tabloları |
| lwIP, netif, olay döngüsü, mDNS, SNTP | −18 | |
| IDF görev yığınları | −25 | |
| Bizim yığınlar (`app_core` 6, `delivery` 10, `sys_worker` 8) | −24 | |
| httpd (8 KB yığın + 7 soket) | −12 | |
| LittleFS (`/data`) + evlog halkası | −6 | |
| **Boşta** | **≈ 300** | |
| TLS oturumu (16+4 KB tampon + el sıkışma + sertifika zinciri) | −45…50 | bir anda tek |
| HTTP isteği (2 KB gövde cJSON ×3 + 14 KB yanıt) | −20 | en büyük uçlar |
| **En yoğun an** | **≈ 230–240** | 16 KB bitişik blok şart |

### 5.2 Öneri

1. **Ölçüm adım 0 ve her adımın denetiminde**: `heap_caps_get_info(MALLOC_CAP_DEFAULT)` → `total_free`, `minimum_free`, `largest_free_block`; `/api/device`'a `heap_largest` eklenir; `delivery` her iş sonunda bu üçünü evlog'a (`lvl=info`, günde en çok 48 satır). Eşik: `minimum_free < 100 KB` ya da `largest < 32 KB` → tasarım gözden geçirilir.
2. **Statik tamponlar**: `delivery`'de ek parçası 4 KB (`MBEDTLS_SSL_OUT_CONTENT_LEN` ile aynı → bir parça = bir TLS kaydı; A1'deki 570 B'lik 965 kaydın yerine ~140 kayıt), base64 çıkış 5,5 KB, SMTP satır tamponu 1 KB — hepsi `static`, `delivery` tek olduğu için paylaşım sorunu yok. HTTP yanıtı için `cJSON_PrintPreallocated` + 16 KB statik yanıt tamponu (`http` bileşeninde, httpd tek görev). Heap yalnız TLS ve cJSON ağacı için kalır → 16 KB bitişik blok her zaman bulunur.
3. **Dinamik mbedTLS tamponu kapalı** (plan aynen). Bir anda tek TLS oturumu olduğundan kazanç ~25 KB, bedeli parçalanma riski. Yalnız ölçüm 1'deki eşik aşılırsa `MBEDTLS_DYNAMIC_BUFFER=y` + `DYNAMIC_FREE_CONFIG_DATA=y` denenir; `MBEDTLS_SSL_IN_CONTENT_LEN` **16384 kalır** (sunucular 16 KB kayıt gönderebilir; MFL uzantısı çoğu SMTP/HTTPS sunucusunda yok).
4. WiFi: `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` 32→16, `DYNAMIC_TX_BUFFER_NUM` 32→16 (tepe ~25 KB düşer; throughput ürün için önemsiz), statik RX 10 kalır; IRAM seçenekleri **açık kalır** (§7 R8).
5. cJSON: 2 KB gövde için ağaç ~3× (yaklaşık 6 KB), parse → doğrula → `cfg_t`'ye kopyala → `cJSON_Delete` tek fonksiyonda; ağaç ömrü ms düzeyinde (plan §8 soru 12 için girdi).

### 5.3 Risk

- Tahmin 50 KB sapabilir; ölçüm olmadan `sdkconfig` kararı verilmez.
- 72 saatlik yeniden denemede parçalanma: statik tamponlar bunu büyük ölçüde keser; kalan risk `largest_free_block` izlemesiyle görünür.

## 6. Yapılandırma şema tablosu

### 6.1 Bulgu

- **X-macro**: tek liste → struct + tablo + (de)serileştirme; öğretici örnek Wikibooks (<https://en.wikibooks.org/wiki/C_Programming/X_macros_and_serialization>). Artı: araç yok, tek dosya. Eksi: derleyici hataları makro satırına işaret eder, IDE gezinmesi ve `-Werror` çıktıları okunmaz, GUI derlemesi (Python) C makrosunu okuyamaz.
- **`offsetof` tanımlayıcı tablosu**: Zephyr `include/zephyr/data/json.h` her alanı `{ad, tip, offsetof(struct, alan), sınır}` ile tanımlar; parse/encode aynı tablodan (<https://github.com/zephyrproject-rtos/zephyr/blob/main/include/zephyr/data/json.h>, `JSON_OBJ_DESCR_PRIM` makroları). JsonX (cJSON üstü, düz tanımlayıcı dizisi, heap'siz; <https://dev.to/embedmind/jsonx-mapping-json-to-c-structs-on-embedded-systems-5e0o>) ve struct2json aynı kalıp. Zephyr `settings` alt sistemi ise ağaç + işleyici (get/set/commit/export) modeli, kalıcılık arka ucundan bağımsız (<https://docs.zephyrproject.org/latest/doxygen/html/group__settings.html>) — bizim "bölüm başına blob" yaklaşımımız buna denk.
- Planın ilkesi 4: NVS, JSON, API, **GUI formu**, doğrulama aynı tablodan. GUI zaten Python ile derleniyor (`GUI/build.py`); IDF'nin kendisi Python gerektiriyor → üreteç ek araç zinciri getirmez.

### 6.2 Öneri — şöyle değiştir: küçük üreteç, X-macro değil

`2611/HW/components/platform/cfg/cfg_schema.json` (elle yazılır, okunur): bölüm → alan → `{tip, sınır, varsayılan, gizli, nvs_key, gui: {sayfa, sıra, i18n_anahtar}}`. `tools/gen_cfg.py` (≈150 satır) üretir:
- `cfg_schema.gen.h`: `cfg_t` (bölüm struct'ları), `cfg_section_t`/`cfg_field_t` enum'ları, `CFG_SCHEMA_VERSION`;
- `cfg_schema.gen.c`: `const cfg_field_desc_t cfg_fields[]` (`offsetof`, `sizeof`, tip, min/max, varsayılan işaretçisi, `secret`), bölüm tablosu (`nvs_key`, blob boyutu);
- `GUI/src/schema.gen.json`: alan sınırları, sıra, i18n anahtarları → istemci doğrulaması ve form üretimi.

CMake `add_custom_command(OUTPUT … DEPENDS cfg_schema.json gen_cfg.py)`, çıktı `build/` altında (depoya girmez; sapma olmaz). Çapraz alan kuralları (ör. `warn_count × aralık < süre`, boş `ip` = DHCP) elle yazılmış `cfg_validate_<bölüm>()` işlevleri; şema bunları adla bağlar. Host testi: her alan için JSON → struct → JSON gidiş-dönüş + `_Static_assert(sizeof(cfg_grp_t) <= 4000)` (NVS parça sınırı; 2048 gövde + 512 alıcı + 128 konu + 64 ad ≈ 2,8 KB).

```c
typedef enum { CF_BOOL, CF_U8, CF_U16, CF_U32, CF_STR, CF_ENUM } cf_type_t;
typedef struct {
    const char *name;  cf_type_t type;  uint16_t off;  uint16_t size;   /* offsetof/sizeof */
    int32_t min, max;  const void *def;  uint8_t section;  uint8_t flags; /* CF_SECRET, CF_REBOOT */
} cfg_field_desc_t;
/* üretilmiş satır örneği: */
/* { "port", CF_U16, offsetof(cfg_smtp_t, port), 2, 1, 65535, &def_smtp_port, CFG_SEC_SMTP, 0 }, */
esp_err_t cfg_json_to_section(uint8_t sec, const cJSON *in, void *out, cfg_err_t *err);  /* tablo güdümlü */
cJSON   *cfg_section_to_json(uint8_t sec, const void *in, bool mask_secrets);
```

X-macro yalnız üreteç istenmezse yedek: aynı tabloyu `CFG_FIELDS(X)` listesinden üretmek mümkündür, ama GUI şeması elle çoğaltılır (ilke 4 kırılır).

### 6.3 Risk

- Üreteç hatası sessiz uyumsuzluk üretir → gidiş-dönüş host testi zorunlu, `-Werror` üretilmiş dosyalara da uygulanır.
- Şema sürümü değişince NVS blob düzeni değişir; `meta.schema_version` farklıysa bölüm varsayılana döner ve evlog'a yazılır (karar 10 ile uyumlu, S4 ilkesi).

## 7. Güç kısıtı tasarımı

### 7.1 Bulgu

- Brownout dedektörü C6: seviye seçimi `ESP_BROWNOUT_DET_LVL_SEL_7..2` = 2,51 / 2,64 / 2,76 / 2,92 / 3,10 / 3,27 V, varsayılan **SEL_7 = 2,51 V** (en toleranslı); Kconfig notu: "gerilim değerleri tahminidir" (`IDF:components/esp_hw_support/power_supply/port/esp32c6/Kconfig.power:13-36`). Eşiği yükseltmek reset'i **artırır**; amaç nedenin doğru sınıflanması (ALTE `CONFIG_ESP_BROWNOUT_DET=y`, evlog `brownout`).
- `esp_pm`: `PM_ENABLE` varsayılan kapalı (`IDF:components/esp_pm/Kconfig:12-17`); C6'da CPU 80/160 MHz (`IDF:docs/en/api-reference/system/power_management.rst:58`); WiFi `esp_wifi_start`–`stop` arasında `ESP_PM_APB_FREQ_MAX` kilidini tutar, modem uyku kapalıysa hiç bırakmaz (`:117`); DFS kesme gecikmesini 40 µs'ye kadar artırır (`:26`). TLS el sıkışması CPU'ya bağlı → 80 MHz'de ~2× uzar, WiFi daha uzun süre açık kalır; 160→80 MHz tasarrufu (~10–15 mA) TX tepe akımının (yüzlerce mA) yanında küçük. Otomatik light-sleep AP modunda zaten etkisiz ve sayacı RTC RC saatine düşürür (§3).
- Flash yazımı: dahili NVS/OTA yazımında sistem 20 ms dilimlerle durur (§1.1); WiFi sürücüsü IRAM seçenekleri açıkken (`esp_wifi/Kconfig:274-300`) MAC/ISR çalışmaya devam eder (beacon/ACK kaçırmaz). Harici SPI flash (LittleFS) dahili cache'i etkilemez; akımı (~20 mA program/silme) önemsiz.
- PHY TX gücü tavanı `ESP_PHY_MAX_WIFI_TX_POWER` varsayılan 20 dBm (`IDF:components/esp_phy/Kconfig:84-88`); ALTE'nin yumuşak başlangıcı (8 dBm) `esp_wifi_start` **sonrası** `esp_wifi_set_max_tx_power` ile uygulanır; aradaki pencerede tavan Kconfig'tir.
- ALTE kapanış nedeni sınıflandırması ve `short_boots` sayacı var (`01-harita/06-cihaz-ve-urun.md:47-51`); brownout → yeniden başlatma → hemen TLS → brownout döngüsü yazılımla kırılabilir.

### 7.2 Öneri — plan için somut kurallar

| # | Kural | Nerede |
|---|---|---|
| R1 | Bir anda tek TLS oturumu; testler de aynı kuyruktan | plan aynen (§4.6) |
| R2 | Röle **enerjilenme** anı ile TLS `connect()` aynı saniyeye düşmez: `app_core` röle geçişinde `power_quiet_until = now + 1 s` yazar; `delivery` bağlantıdan önce bu damgayı okur, gerekirse bekler. Röle önceliklidir (fiziksel eylem), teslimat 1 s gecikebilir | `relay_sm` + `delivery` |
| R3 | Tetikleme bekleyen iş varken OTA başlamaz (plan: abort); OTA sırasında teslimat başlamaz (`sys_worker` OTA'ya girerken `EV_OTA_BUSY`, `delivery` bekler) | `sys_worker` ↔ `delivery` |
| R4 | `esp_pm` **kapalı**, modem uyku kapalı (ALTE), `CONFIG_FREERTOS_HZ=100` | `sdkconfig.defaults` |
| R5 | Brownout eşiği varsayılan (2,51 V); `CONFIG_ESP_BROWNOUT_DET=y` | `sdkconfig.defaults` |
| R6 | `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=17` (ürün tavanı; başlangıç penceresini kapatır); yumuşak başlangıç `esp_wifi_start` hemen ardından | `net_wifi` |
| R7 | **Brownout sonrası düşük güç açılışı**: son reset `brownout`/`pwr_glitch` **ve** önceki açılış <60 s ise bu açılışta TX tavanı 11 dBm, teslimat `EV_NET_UP` + 30 s sonra, ekler atlanır (gövdeye not, karar 9); üç ardışık kısa brownout açılışında 8 dBm. Normal açılış sayacı sıfırlar; her kademe evlog'a | `main.c` + `net_wifi` + `delivery` |
| R8 | WiFi IRAM seçenekleri (`ESP_WIFI_IRAM_OPT`, `RX_IRAM_OPT`, `EXTRA_IRAM_OPT`) **açık** kalır; `SPI_FLASH_AUTO_SUSPEND=n` (belge: Wi-Fi ile kullanma) | `sdkconfig.defaults` |
| R9 | Gönderim sırasında NVS yazımı yalnız `rt` (alıcı başına bir kez, §2.2); `cfg` yazımı httpd'den gelir, kullanıcı eylemidir, engellenmez | kod incelemesi maddesi |

### 7.3 Risk

- R7 yanlış pozitif: güç kesintisi (brownout değil, `power_loss`) ile karışırsa teslimat gereksiz 30 s gecikir — sınıflandırma zaten ayrı (`brownout` ≠ `power_loss`); 30 s ürün için önemsiz.
- R2 `inverted` rölede "enerjilenme" = pin düşmesi; `relay_sm` fiziksel seviyeyi değil **bobin durumunu** bildirir (tek yerde çevrim).

## Plana etkisi

1. **§2 ilke 1 / §3.2**: `app_core` "asla bloklamaz" → "flash dışında bloklamaz; `rt` blob yazımı sınırlı (ölçülür, >200 ms evlog), geçişler senkron, 60 s kontrol noktası gerekirse `sys_worker`'a". Kuyruk dolu politikası tabloya girer (§1.2). `app/core` CMake `REQUIRES` beyaz listesi.
2. **§3.3 / §4.1**: `EV_TICK` "sayaç azalt" değil "değerlendir"; `timer_sm`, `relay_sm`, `warn_sched`, düğme penceresi son tarih aritmetiğiyle (`now_fn` enjekte). `rt` blob'a `trigger_seq`, `since_trigger_s`, `attempts[]`, `early_pending` eklenir; `deadline` asla yazılmaz.
3. **§4.6**: `TRIGGER` 72 sa (açık süre) ya da ertelemeye kadar, ±%10 jitter, 30 dk'da bir; `EARLY_WARN` 3 deneme (0/1/5 dk); kalıcı/geçici hata tablosu; `pending` biti yalnız `DATA 250`'de temizlenir; sabit `Message-ID`, webhook `event_id`, MQTT `seq`; `EV_NET_UP` + 10 s yerleşme.
4. **§4.4 / §3.4**: açılış sırası adım 2 öne çekilir: röle pini NVS'ten **önce** (varsayılan polarite), `cfg_load` sonrası düzeltme; `gpio_reset_pin` yasak; donanım pull direnci üretim notu + GUI `inverted` uyarısı.
5. **§3.2 bekçi satırı**: `app_core` ve `sys_worker` TWDT abonesi; `delivery` uygulama bekçisi (`app_core` izler, `RST_DELIVERY_HANG`). `evlog` reset nedenlerine `RST_DELIVERY_HANG` eklenir.
6. **§5.8 sdkconfig**: `ESP_WIFI_DYNAMIC_RX/TX_BUFFER_NUM=16`, `ESP_PHY_MAX_WIFI_TX_POWER=17`, `PM_ENABLE=n`, `SPI_FLASH_AUTO_SUSPEND=n`, `MBEDTLS_DYNAMIC_BUFFER=n`, `ESP_TASK_WDT_TIMEOUT_S=10` + `PANIC=y` (ALTE), `FREERTOS_HZ=100`.
7. **§6.5 denetim**: `/api/device`'a `heap_largest`; adım 0'da heap ölçüm tablosu (§5.1 tahmini doğrulanır); `q_drop`, `core_slow`, `rt` yazım süresi evlog maddeleri; teslimat sonrası heap satırı.
8. **§5.1 cfg**: X-macro yerine `cfg_schema.json` + `tools/gen_cfg.py` → `cfg_schema.gen.h/.c` + `GUI/src/schema.gen.json`; gidiş-dönüş host testi; `_Static_assert` blob ≤ 4000 B.
9. **§7 adım 0–1**: `main.c`'ye brownout sonrası düşük güç açılışı (R7); `net_wifi`'ye TX tavanı kademesi; `delivery`'ye `power_quiet_until` (R2) ve `EV_OTA_BUSY` (R3).
10. **`yapilacaklar.md`**: uyku modu açılacaksa harici 32 kHz kristal (`RTC_CLK_SRC_EXT_CRYS`) ve sayaç etkisi; ek içeriği değişen yeniden gönderimde `Message-ID` eki.
