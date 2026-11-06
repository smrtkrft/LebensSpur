# 01 — Çekirdek: yaşam döngüsü, sayaç, çıkışlar

Kaynaklar (tamamı okundu): `ALTE/HW/main/main.c` (528 satır), `components/timer_manager/{timer_manager.c,include/timer_manager.h}` (506+79), `components/output/{output.c,include/output.h}` (456+71), `components/system_info/*` (85+21), `components/ls_util/*` (111+27), `ALTE/HW/CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults`, `main/CMakeLists.txt`, `main/idf_component.yml`. Diğer bileşenlere (web_server, sys_log, smtp_client, telegram_client, api_client, ext_flash, wifi_manager) yalnızca bu modüllerle temas noktaları için bakıldı. Etkin yapılandırma değerleri `esp_bin/HW/sdkconfig`'den, ESP-IDF iç davranışı `~/esp/esp-idf-v5.5.2` kaynağından doğrulandı.

Satır referansları `dosya:satır` biçimindedir; dosya adları `ALTE/HW/` köküne göredir (`main.c` = `main/main.c`, `timer_manager.c` = `components/timer_manager/timer_manager.c`, `output.c` = `components/output/output.c`, `web_server.c` = `components/web_server/web_server.c`, `sys_log.c` = `components/sys_log/sys_log.c`).

## 0. Tek bakışta

```
                       esp_timer görevi (öncelik 22, yığın 3584 B)
                       ┌────────────────────────────────────────────┐
  1 Hz "countdown"  →  │ timer_tick_cb (timer_manager.c:160)         │
                       │   tatil sayacı  → bitince ana sayaç başlar  │
                       │   ana sayaç --                              │
                       │   eşik geçildi → s_alarm_cb(idx)  ─────────┼─→ on_alarm_triggered (main.c:212)
                       │   0'a indi     → save_to_nvs, s_expiry_cb ──┼─→ on_timer_expired   (main.c:342)
                       │   her 2 sn     → s_tick_cb  ────────────────┼─→ web_server_send_timer_event (SSE)
                       └────────────────────────────────────────────┘
  on_alarm_triggered: sys_log → görevler: alarm_mail(10 KB) | alarm_tg(8 KB) | [alarm_api(8 KB)]
  on_timer_expired  : sys_log → [röle: output_relay_trigger → relay_wrk(3 KB)] → SSE → görevler: trig_mail(10 KB) | [trig_api(8 KB)]

  Erteleme (check-in) üç yoldan timer_manager_restart()'a varır:
    fiziksel düğme GPIO18 → ISR → kuyruk → btn_task(öncelik 10) → on_button_pressed (main.c:79)  [röleyi de kapatır]
    web  POST /api/timer/set {"action":"restart"}        → web_server.c:956                       [röleye dokunmaz]
    uzak GET  /api/reset?key=ls_xxxxxxxx                 → web_server.c:2050                      [röleye dokunmaz]
```

---

## 1. `components/timer_manager` — geri sayım, tatil modu, erken uyarı eşikleri

### 1.1 Modül amacı

Tek bir `esp_timer` ile saniye çözünürlüklü, NVS'de kalıcı geri sayım tutar. Ana sayacın yanında ayrı bir "tatil" sayacı vardır; tatil sürerken ana sayaç dondurulur, tatil bitince ana sayaç kendiliğinden tam süreden başlar. Erken uyarı eşiklerini hesaplar ve eşik geçildiğinde, süre dolduğunda ve her 2 saniyede bir kayıtlı geri çağırmaları (callback) çağırır. Mail/Telegram/röle gibi eylemleri bilmez; yalnızca callback verir.

### 1.2 Dış arayüz (`timer_manager.h`)

| Fonksiyon | Ne yapar | Dönüş / hata | Çağıran bağlam |
|---|---|---|---|
| `esp_err_t timer_manager_init(void)` (`timer_manager.c:298`) | NVS'den durumu yükler, açılışta geçmiş alarmları yeniden hizalar (`resync_alarms_fired`), gerekiyorsa donanım zamanlayıcısını başlatır | Her zaman `ESP_OK` (NVS açılamazsa sıfır değerlerle devam eder, `:107-116`) | `app_main` (`main.c:460`) |
| `esp_err_t timer_manager_set(const char *unit, int value)` (`:322`) | Yeni süre: `unit` ∈ {`"gun"`,`"saat"`,`"dakika"`}, `value` 1..60. Sayaç tam süreden, etkin olarak başlar; alarm sayacı sıfırlanır; alarm sayısı `value/2` ile sınırlanır (kalıcı); NVS'ye yazar | `ESP_ERR_INVALID_ARG` (birim/değer); yoksa `ESP_OK` | httpd (`web_server.c:1023`, `POST /api/timer/set`) |
| `timer_state_t timer_manager_get_state(void)` (`:367`) | 7 alanın kilitsiz anlık kopyası | değer | httpd (`web_server.c:917,959,979,1027,1393`), btn_task (`main.c:94`), esp_timer görevi (`main.c:215`), app_main (`main.c:520`) |
| `esp_err_t timer_manager_cancel(void)` (`:380`) | Donanım zamanlayıcısını durdurur, `enabled=false`, `remaining=0`, NVS'ye yazar. `total` korunur (yeniden başlatma mümkün kalır) | `ESP_OK` | httpd (`web_server.c:950`) |
| `esp_err_t timer_manager_restart(void)` (`:392`) | `remaining=total`, `enabled=true`, alarm sayacı sıfır, NVS, zamanlayıcı yeniden kurulur | `ESP_ERR_INVALID_STATE` (`total<=0`, yani hiç süre ayarlanmamış) | btn_task (`main.c:89`), httpd (`web_server.c:956` web düğmesi, `web_server.c:2050` uzaktan API) |
| `esp_err_t timer_manager_set_vacation(int days)` (`:418`) | 1..60 gün. Ana sayaç `remaining=total`, `enabled=false` (dondurulur); tatil sayacı `days*86400` sn ile başlar | `ESP_ERR_INVALID_ARG` | httpd (`web_server.c:975`) |
| `esp_err_t timer_manager_cancel_vacation(void)` (`:447`) | Tatili kapatır; `total>0` ise ana sayacı tam süreden etkinleştirir | `ESP_OK` | httpd (`web_server.c:991`) |
| `esp_err_t timer_manager_set_alarm_count(int count)` (`:473`) | 0..10. Sayaç çalışıyorsa eşikleri hemen yeniden hizalar (geçmiş eşikler atlanır) | `ESP_ERR_INVALID_ARG` | httpd (`web_server.c:1002`) |
| `void timer_manager_set_expiry_callback(void (*cb)(void))` (`:493`) | Süre dolunca çağrılacak işlev | — | `app_main` (`main.c:475`) |
| `void timer_manager_set_alarm_callback(void (*cb)(int alarm_index))` (`:503`) | Erken uyarı eşiği geçilince; `alarm_index` 0 tabanlı | — | `app_main` (`main.c:476`) |
| `void timer_manager_set_tick_callback(void (*cb)(void))` (`:498`) | Her `SSE_SYNC_INTERVAL`=2 saniyede bir ve tatil bitiminde | — | `app_main` (`main.c:477`, doğrudan `web_server_send_timer_event`) |

Üç callback de **esp_timer görevinin bağlamında, eşzamanlı (senkron)** çağrılır (`timer_manager.c:218,227,183,200,243`). Başlık dosyası "kısa tutun" der (`timer_manager.h:64,71,78`); main.c'nin buna uymadığı §8'de.

`timer_state_t` (`timer_manager.h:7-15`): `enabled`, `remaining_seconds`, `total_seconds`, `vacation_enabled`, `vacation_remaining`, `vacation_total`, `alarm_count`.

### 1.3 Davranış

**Durum değişkenleri** (`timer_manager.c:29-52,158`): hepsi `static volatile`, **mutex/kritik bölge yok**. `s_enabled`, `s_remaining`, `s_total`, `s_vacation_enabled`, `s_vacation_remaining`, `s_vacation_total`, `s_alarm_count` (varsayılan 3), `s_save_counter`, `s_alarms_fired`, `s_next_alarm_threshold`, `s_tick_counter`.

**Türetilmiş durum makinesi** (kodda açık bir enum yok; değişken kombinasyonlarından çıkarıldı):

| Durum | Koşul | Nasıl girilir | Nasıl çıkılır |
|---|---|---|---|
| BOŞ | `total==0` | fabrika / ilk açılış | `set` |
| ÇALIŞIYOR | `enabled && remaining>0 && !vacation` | `set`, `restart`, `cancel_vacation`, tatil bitişi | tik ile 0'a iner → DOLDU; `cancel`; `set_vacation` |
| DURDU | `!enabled && remaining==0 && total>0` | `cancel` **veya süre dolması** (ikisi aynı görünür, ayrı bayrak yok) | `restart`, `set` |
| TATİL | `vacation_enabled && vacation_remaining>0` | `set_vacation` | tik ile 0'a iner → ÇALIŞIYOR (`:168-185`); `cancel_vacation` |

Tatil sürerken ana sayaç alanları (`enabled`, `remaining`) `restart`/`set` ile değiştirilebilir ama tik işlemez (`:165-203` erken `return`); yani GUI "etkin" gösterirken sayaç ilerlemez.

**Zamanlama**: tek `esp_timer`, ad `"countdown"`, periyot 1 000 000 µs (`:255-266`). Yalnızca ana ya da tatil sayacı gerektiğinde yaşar (`ensure_hw_timer` `:282-292`); `set/restart/set_vacation` her seferinde durdurup silip yeniden yaratır (`:340,359`). Callback'ler `ESP_TIMER_TASK` dağıtımıyla esp_timer görevinde koşar: öncelik `ESP_TASK_PRIO_MAX-3 = 22` (`esp_task.h:46`), yığın `CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584` (`esp_bin/HW/sdkconfig:1328`). `CONFIG_PM_ENABLE` kapalı → hafif uyku yok, tik sapması yok. Tik sayımı duvar saatine bağlı değildir: **cihaz kapalıyken geçen süre sayılmaz**, SNTP/RTC kullanılmaz.

**Tik algoritması** (`timer_tick_cb` `:160-245`):
1. Tatil etkinse: `vacation_remaining--`; 0'a inince tatil kapanır, `total>0` ise ana sayaç `remaining=total, enabled=true, alarms_fired=0`, eşik yeniden hesaplanır, NVS yazılır, tick_cb çağrılır, dön (`:165-185`). Aksi halde 60 tikte bir NVS, 2 tikte bir tick_cb; dön.
2. `!enabled || remaining<=0` → dön (`:206`). Not: süre dolduktan sonra esp_timer silinmez, 1 Hz boşa tiklemeye devam eder.
3. `remaining--`.
4. Alarm: `s_alarm_cb && threshold>0 && remaining>0 && remaining<=threshold` → `idx=fired; fired++; recalc; s_alarm_cb(idx)` (`:211-219`). Tek tikte en fazla bir alarm.
5. `remaining<=0` → `enabled=false, remaining=0`, `save_to_nvs()` (NVS yazımı esp_timer görevinde), `s_expiry_cb()` (`:221-230`), dön.
6. `s_save_counter` 60'a ulaşınca NVS kontrol noktası (`NVS_SAVE_INTERVAL` `:24`, `:232-237`).
7. `s_tick_counter` 2'ye ulaşınca tick_cb (`:240-244`).

**Erken uyarı eşikleri** (`recalc_alarm_threshold` `:57-69`): `threshold = total * (alarm_count − fired) / (alarm_count + 1)`; eşik geçildiğinde alarm ateşlenir. Örnek: 24 saat, 3 alarm → kalan 18 s, 12 s, 6 s'de. `alarm_count==0` ya da tüm alarmlar atıldıysa eşik 0 → alarm yok. Açılışta ve alarm sayısı değişince `resync_alarms_fired` (`:74-82`) kalan süreye göre kaç eşiğin zaten geçildiğini hesaplar, **ateşlemeden** atlar (yorum `:71-73`: eskiden her açılışta mail patlaması oluyordu).

**Sınır değerler**:
- Süre: 1..60 × {gün 86400, saat 3600, dakika 60} → 60 sn … 5 184 000 sn (60 gün); `int32` içinde güvenli. Web katmanı da 1..60'ı denetler (`web_server.c:1018`).
- Tatil: 1..60 gün.
- Alarm sayısı: 0..10; NVS'den 10'dan büyük okunursa 3'e düşer (`:133`).
- **Otomatik kırpma** (`:348-354`): `set` çağrısında `alarm_count > value/2` ise `value/2`'ye indirilir ve **NVS'ye kalıcı yazılır**. `value` saniye değil birim adedidir: "1 gün" → en çok 0 alarm, "2 gün" → 1, "3 saat" → 1, "5 dakika" → 2. Kullanıcı 1 günlük sayaç kurduğunda erken uyarıları sessizce kaybeder ve bir daha sayaç süresini artırsa bile alarm sayısı kendiliğinden geri gelmez.

**Yeniden başlatmada korunan / kaybolan**:
- Korunur (NVS `ls_timer`): etkin bayrağı, kalan, toplam, alarm sayısı, tatil üçlüsü. Kalan süre son kontrol noktasındaki değerdir: en çok 59 tik (≈59 sn) + açılış süresi kadar **fazladan** süre kalır (kullanıcı lehine).
- Kaybolur: `s_alarms_fired` (açılışta `resync` ile türetilir), `s_tick_counter`, `s_save_counter`. "Süre doldu mu, kullanıcı mı iptal etti" ayrımı hiç tutulmaz.
- NVS'de `remaining<=0` okunursa sayaç devre dışı sayılır (`:138-141`); yani dolma anında kaydedilen `enabled=0, remaining=0` açılışta DURDU durumudur, tetikleme **yinelenmez**.

**Yarış durumları ve kilit**:
- Kilit yok. Tek çekirdek, esp_timer görevi (22) httpd (5) ve btn_task (10)'ı her an kesebilir.
- `set/restart/cancel/set_vacation/cancel_vacation` önce `stop_hw_timer` çağırdığı için (`:340,399,382,424,449`) alan güncellemeleri sırasında tik gelemez — fiilî koruma budur. `esp_timer_delete` IDF 5.5.2'de serbest bırakmayı esp_timer görevine erteler (`esp_timer.c:266-293`, `EVENT_ID_DELETE_TIMER`), dolayısıyla callback çalışırken başka görevden silmek güvenlidir.
- `set_alarm_count` (`:473-487`) zamanlayıcıyı **durdurmaz**: `s_alarm_count`, `s_alarms_fired`, `s_next_alarm_threshold` üç ayrı yazımdır; arada tik gelirse alarm yanlış indeksle/eşikle atılabilir (olasılık düşük, pencere mikrosaniye).
- `get_state` kilitsiz 7 alan okur; tik araya girerse tutarsız anlık görüntü olabilir (ör. `enabled=true, remaining=0`).
- Callback'ler bloklarsa (bkz. §8) tik görevi bloklanır; bu sırada httpd `set` çağırırsa `stop_hw_timer` çalışır ama o anki callback zaten yolda devam eder.

### 1.4 Donanım

Yok (yalnızca `esp_timer`).

### 1.5 Kalıcı veri — NVS namespace `"ls_timer"` (`timer_manager.c:16-23`)

| Anahtar | Tip | Varsayılan | Yazan | Okuyan |
|---|---|---|---|---|
| `t_enabled` | u8 | 0 | `save_to_nvs` (`:88-102`) | `load_from_nvs` (`:104-152`) |
| `t_remain` | i32 | 0 | aynı | aynı |
| `t_total` | i32 | 0 | aynı | aynı |
| `t_alarms` | u8 | 3 | aynı | aynı |
| `v_enabled` | u8 | 0 | aynı | aynı |
| `v_remain` | i32 | 0 | aynı | aynı |
| `v_total` | i32 | 0 | aynı | aynı |

`save_to_nvs` her çağrıda 7 `nvs_set` + `nvs_commit` yapar; çağrı noktaları: `set`, `cancel`, `restart`, `set_vacation`, `cancel_vacation`, `set_alarm_count`, süre dolması, tatil bitişi, çalışırken her 60 tik. NVS bölümü 24 KB (`partitions.csv:3`). Yalnızca bu modül yazar/okur; web_server sayaç NVS'sine doğrudan dokunmaz.

### 1.6 Bağımlılıklar

- Çağırdığı: `nvs_flash`, `esp_timer`, `esp_log` (`CMakeLists.txt`: `PRIV_REQUIRES nvs_flash esp_timer`).
- Çağıranlar: `main.c` (init, callback kaydı, restart, get_state), `web_server.c` (set/cancel/restart/vacation/alarm_count/get_state).
- Başka proje bileşenine bağımlı değil; ayrı derlenebilir, sınanabilir.

---

## 2. `components/output` — fiziksel düğme ve röle

### 2.1 Modül amacı

Tek fiziksel düğmeyi (çoklu basma ile üç işlev) ve tek röle çıkışını yönetir. Röle, yapılandırmaya göre gecikme → sürekli ya da darbeli (pulse) → süre sonunda kapanma sırasını ayrı bir FreeRTOS görevinde yürütür. Düğme/röle ayarları NVS'de kalıcıdır.

### 2.2 Dış arayüz (`output.h`)

| Fonksiyon | Ne yapar | Dönüş / hata | Çağıran bağlam |
|---|---|---|---|
| `esp_err_t output_init(void)` (`output.c:254`) | GPIO19'u sıfırlayıp çıkış yapar, NVS'den ayarları yükler, boşta seviyesini basar, GPIO18'i pull-up'lı giriş + düşen kenar kesmesi yapar, 4 elemanlı kuyruk + `btn_task` + ISR kurar | `gpio_config` hataları, `ESP_ERR_NO_MEM` (kuyruk), `gpio_install_isr_service`/`gpio_isr_handler_add` hataları (zaten kurulu `ESP_ERR_INVALID_STATE` yutulur `:304-307`) | `app_main` (`main.c:467`) |
| `void output_set_button_callback(void (*cb)(void))` (`:325`) | 1–2 basma callback'i | — | `app_main` (`main.c:472`) |
| `void output_set_factory_reset_callback(void (*cb)(void))` (`:330`) | 5 basma callback'i | — | `app_main` (`main.c:473`) |
| `void output_set_relay_config(uint16_t duration, uint8_t pulse, uint8_t delay)` (`:335`) | Yalnız RAM'e yazar (doğrulama yok) | — | httpd (`web_server.c:1272`) |
| `void output_get_relay_config(uint16_t*, uint8_t*, uint8_t*)` (`:342`) | RAM'den okur; NULL işaretçiler atlanır | — | httpd (`web_server.c:1239`) |
| `void output_set_inverted(bool)` (`:349`) | Ters mantık; röle kapalıysa boşta seviyesini **hemen** uygular | — | httpd (`web_server.c:1269`) |
| `bool output_get_inverted(void)` (`:358`) | | | httpd |
| `void output_relay_trigger(void)` (`:378`) | Önceki worker'ı durdurur (≤1,5 sn bekler), sys_log'a yazar, `relay_wrk` görevini yaratır | void; `xTaskCreate` dönüşü **denetlenmez** (`:390`) | esp_timer görevi (`main.c:351`), httpd (`web_server.c:1284`, `POST /api/relay/test`) |
| `void output_relay_off(void)` (`:393`) | Worker'ı durdurur, boşta seviyesi, `s_relay_on=false` | void | btn_task (`main.c:86`) |
| `bool output_relay_is_on(void)` (`:401`) | `s_relay_on` | | btn_task (`main.c:85`) |
| `esp_err_t output_load_config(void)` (`:406`) | NVS → RAM; NVS yoksa varsayılanlar | her zaman `ESP_OK` | `output_init` |
| `esp_err_t output_save_config(void)` (`:441`) | RAM → NVS | `nvs_open` hatası | httpd (`web_server.c:1273`) |

### 2.3 Davranış

**Düğme** (`button_isr_handler` `:55-72`, `button_task` `:80-143`):
- ISR: `esp_timer_get_time()` ile 200 ms yazılımsal debounce (`DEBOUNCE_MS` `:25`, `:59-63`), sonra kuyruğa 1 bayt. ISR içinde callback yok (doğru).
- `btn_task` (yığın 4096, öncelik 10, `:300`) çoklu basma sayar:
  - İlk basma: sayaç 1, `first_press_time` (`:92-96`).
  - Sonraki basmayı **400 ms** bekler (`WAIT_TICKS` `:87`). Gelirse ve ilk basmadan 3 sn geçmediyse (`WINDOW_US` `:86`) sayaç artar; **5'e ulaşınca anında** fabrika sıfırlama callback'i (`:106-113`).
  - 400 ms içinde basma gelmezse değerlendirme (`evaluate:` `:120-140`): ≥5 → fabrika (buraya fiilen düşmez), ≥3 → `sys_log_set_restart_reason(SL_RST_USER_BUTTON)` + 100 ms + `esp_restart()` (`:126-131`), aksi (1 **veya 2**) → `s_button_cb` (`:133-137`).
  - Sonuç: tek basmanın etkisi basmadan **400 ms sonra** görülür; çift basma tek basma ile aynı; 3–4 basma yeniden başlatır; 5 basma fabrika sıfırlar.
  - `goto evaluate` dalı (`:114-117`): "pencere dışı" basma eski sayacı değerlendirir ve **yeni basmayı düşürür**; ama 3 sn pencereyi 400 ms aralıklı basmalarla 5'e ulaşmadan aşmak mümkün olmadığından bu dal fiilen erişilemez.
  - Uzun basma, bırakma algılama, basılı tutma yok. Kuyruk 4 derin; daha hızlı basmalar düşer.
- Callback'ler `btn_task` içinde koşar (`on_button_pressed`, `on_factory_reset` — bkz. §6).

**Röle worker** (`relay_worker_task` `:148-252`, yığın 3072, öncelik 5):
- Başlangıçta ayarların anlık kopyasını alır (`:152-154`); sonradan değişen ayar koşan worker'ı etkilemez.
- `delay>0`: tek `vTaskDelay(delay*1000)` (`:157-160`), **kesilemez**; sonra iptal bildirimi kontrol edilir (`:163`).
- `pulse==0` (sürekli): röle aktif, `s_relay_on=true`; `duration>0` ise saniye saniye bekleyip iptali yoklar, süre bitince boşta (`:173-184`); `duration==0` ise **görev biter, röle açık kalır** (`:185-188`).
- `pulse>0` (darbeli): `pulse` sn açık / `pulse` sn kapalı döngüsü, `duration` sn toplam (`duration==0` → sınırsız `0xFFFFFFFF` `:192`); son fazlar kırpılır (`:204-206,224-226`); kapalı fazda `s_relay_on=false` (`:220-221`).
- `done:` (`:243-251`): açıksa kapat, `s_pulse_task_handle=NULL`, kendini sil.
- `stop_relay_worker` (`:364-376`): `xTaskNotifyGive` + 30×50 ms = en çok 1,5 sn bekler; çıkmazsa uyarı basıp **tutamacı NULL'lar** (worker aslında koşmaya devam eder, bir sonraki yoklamada çıkar). Gecikme fazındaki worker 1,5 sn'de çıkamaz (tek uzun `vTaskDelay`), bu durum her gecikmeli iptalde yaşanır.
- `output_relay_trigger` iki worker'ın aynı anda röleyi sürmesini önlemek için önce eskisini durdurur (`:380-381`) — bu bekleme çağıranın görevinde (tetiklemede **esp_timer görevinde**) geçer.

**Sınır değerler**: `output.c` doğrulamaz; `web_server.c:1262` sınırlar: `duration` 0..3600 sn, `pulse` 0..60 sn, `delay` 0..60 sn. Tipler u16/u8/u8.

**Yeniden başlatmada**: ayarlar NVS'den gelir; **röle durumu gelmez** — açılışta röle her zaman boşta seviyesine çekilir (`:274`). Tetiklenmiş ve sürekli açık bir röle, cihaz yeniden başlarsa söner.

### 2.4 Donanım

| Öğe | Değer | Kaynak |
|---|---|---|
| Düğme | `GPIO_NUM_18` (XIAO D10), giriş, dahili pull-up, `GPIO_INTR_NEGEDGE`, **aktif LOW** (GND'ye basar) | `output.c:21,279-285` |
| Röle | `GPIO_NUM_19` (XIAO D8), çıkış, pull yok, **aktif HIGH** (varsayılan); `relay_inv=1` ise tersi | `output.c:22,45-46,260-266` |
| C6 notu | GPIO19–23 açılışta SDIO işlevine atanmıştır; `gpio_reset_pin(19)` önce çağrılır | `output.c:256-257` |
| Debounce | 200 ms, ISR içinde zaman damgasıyla | `output.c:25,59-63` |

**Güç açılışında güvenli durum**: pin, `app_main` → `nvs_flash_init` → `ext_flash_init` → `sys_log_init` → `timer_manager_init` → `output_init` sırasına kadar sürülmez; `output_init` içinde önce `gpio_config` (çıkış, henüz seviye yazılmadı), sonra NVS'den `relay_inv` okunup boşta seviyesi basılır (`:267-274`). `inverted=false` için boşta = LOW, açılış pencereyle uyumludur. `inverted=true` (boşta enerjili) bağlantıda bu pencere boyunca pin LOW/sürülmemiş olduğundan röle **enerjisiz = "tetiklenmiş" görünür**; her yeniden başlatmada kısa bir yanlış-tetik darbesi oluşur. Kod bunu ele almaz.

### 2.5 Kalıcı veri — NVS namespace `"ls_output"` (`output.c:28-32`)

| Anahtar | Tip | Varsayılan | Yazan | Okuyan |
|---|---|---|---|---|
| `relay_inv` | u8 | 0 | `output_save_config` (httpd) | `output_load_config` (`output_init`) |
| `relay_dur` | u16 | 0 (süresiz) | aynı | aynı |
| `relay_pulse` | u8 | 0 (darbe yok) | aynı | aynı |
| `relay_delay` | u8 | 0 | aynı | aynı |

### 2.6 Bağımlılıklar

- Çağırdığı: `driver` (gpio), `esp_timer` (zaman damgası), `nvs_flash`, `freertos` (queue/task/notify), `esp_system` (`esp_restart`), **`sys_log`** (`sys_log_set_restart_reason`, `sys_log_write`, `sys_log_mark_event` `:128-129,385-387`). `CMakeLists.txt`: `PRIV_REQUIRES sys_log driver esp_timer nvs_flash log esp_system`.
- Çağıranlar: `main.c` (init, callback kaydı, trigger/off/is_on), `web_server.c` (get/set config, inverted, save, test trigger).
- `sys_log` bağımlılığı modülü yalnız başına derlenemez kılar; "3x = yeniden başlat" kararı da bu modülün içindedir (`:126-131`), main.c'ye callback verilmez.

---

## 3. `components/system_info` — sistem özeti

### 3.1 Modül amacı

Web arayüzünün "sistem ayrıntısı" sayfası için çip, flash, heap, çalışma süresi, RSSI, sıfırlama nedeni ve firmware sürümünü tek yapıda toplar.

### 3.2 Dış arayüz

`esp_err_t system_info_get(system_info_t *info)` (`system_info.c:13-85`): `info==NULL` → `ESP_ERR_INVALID_ARG`; yoksa `ESP_OK`. Tek çağıran: httpd `api_system_detail_handler` (`web_server.c:1131`, `GET /api/system/detail`). `system_info_t` (`system_info.h:6-19`): `chip_model[16]`, `cores`, `cpu_freq_mhz`, `flash_size_kb`, `flash_used_kb`, `heap_total`, `heap_free`, `heap_min_free`, `uptime_seconds`, `wifi_rssi`, `reset_reason[32]`, `fw_version[16]`.

### 3.3 Davranış

- `cpu_freq_mhz` derleme sabiti `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` (160) (`:32`).
- `flash_used_kb`: etiketi `slot_a`/`slot_b`/`user_data` olmayan tüm bölümlerin boyut toplamı (`:39-52`). Bu etiketler dahili bölüm tablosunda yoktur (harici flash mount adlarıdır), dolayısıyla toplam **daima sabittir** (≈4 028 KB): "kullanılan" değil "ayrılmış" alan. Yanıltıcı metrik.
- `esp_partition_iterator_release(it)` döngü sonunda `it==NULL` ile çağrılır (`:51`); IDF'de zararsız.
- RSSI: STA bağlı değilse 0 kalır (`:63-66`). Sıfırlama nedeni İngilizce sabit metin (`:69-79`). Sürüm `esp_app_get_description()->version` = `PROJECT_VER` (`:82`).

### 3.4 Donanım / 3.5 Kalıcı veri

Yok / yok.

### 3.6 Bağımlılıklar

`esp_hw_support`, `esp_wifi`, `esp_timer`, `spi_flash`, `heap`, `log`, `esp_partition`, `app_update` (CMake). Proje bileşenine bağımlı değil. Çağıran: yalnız `web_server`.

---

## 4. `components/ls_util` — JSON yardımcıları

### 4.1 Modül amacı

cJSON üstünde küçük, güvenli alan okuyucular; JSON string kaçışlama; SMTP başlığı için CR/LF temizleme. Projede "tek doğru JSON ayrıştırıcı" olması amaçlanmış (`ls_util.h:4-5`).

### 4.2 Dış arayüz (`ls_util.h`)

| Fonksiyon | Davranış | Dönüş |
|---|---|---|
| `int ls_json_get_str(json, key, out, out_size)` (`ls_util.c:7-36`) | string → kopya; sayı → `%d` ile metin (double→int kesme); bool → `"true"/"false"`; `out_size-1`'e sessiz kırpma | uzunluk; yoksa/bozuksa −1 |
| `bool ls_json_get_int(json, key, int *out)` (`:38-55`) | sayı ya da `strtol` ile sayısal string (`"12abc"` → 12 kabul edilir) | bulundu mu |
| `bool ls_json_get_bool(json, key, bool *out)` (`:57-74`) | bool; sayı≠0; string `"true"`/`"1"` | bulundu mu |
| `size_t ls_json_escape(src, dst, dst_size)` (`:76-103`) | `" \ \n \r \t` kaçışlanır; **diğer kontrol karakterleri (<0x20) sessizce atılır** (`:97`); sığmazsa keser (`:94`) | yazılan uzunluk; `dst` her zaman NUL'lu |
| `void ls_strip_crlf(char *s)` (`:105-111`) | CR/LF → boşluk, yerinde | — |

Hepsi çağıranın görevinde koşar; durumsuz, yeniden girilebilir.

### 4.3 Davranış notu

Her `ls_json_get_*` çağrısı **tüm gövdeyi yeniden ayrıştırır** (`cJSON_Parse` + `cJSON_Delete`). `web_server.c` 20 noktada kullanır; ör. `api_relay_set_handler` 256 baytlık gövdeyi 4 kez ayrıştırır (`web_server.c:1259-1268`). Küçük gövdelerde önemsiz, ama tasarım olarak "parse once, read many" değil.

### 4.4–4.5 Donanım / kalıcı veri: yok.

### 4.6 Bağımlılıklar

`REQUIRES json` (cJSON). Çağıranlar: `web_server.c` (20), `telegram_client.c` (1), `api_client` (CMake'de bağımlı). `main.c`, `timer_manager`, `output` kullanmaz.

---

## 5. Yapı dosyaları (bu alanı ilgilendiren kısımlar)

**`ALTE/HW/CMakeLists.txt`**: `SDKCONFIG_DEFAULTS` açıkça `sdkconfig.defaults` (`:4`; derleme `esp_bin/HW` sarmalayıcı projesinden). `PROJECT_VER "0.0.70"` (`:7`) → `esp_app_desc_t.version` → OTA karşılaştırması, `system_info.fw_version`, açılış logu (`main.c:514-516`).

**`main/CMakeLists.txt`**: `PRIV_REQUIRES wifi_manager web_server ext_flash timer_manager smtp_client telegram_client api_client sys_log output nvs_flash spi_flash esp_partition gui_ota fw_ota app_update esp_timer`. **`main/idf_component.yml`**: `espressif/mdns` (main'de kullanılmıyor; wifi_manager için).

**`partitions.csv`** (dahili 4 MB, fabrika bölümü yok):

| Ad | Tip | Ofset | Boyut |
|---|---|---|---|
| `nvs` | data/nvs | 0x9000 | 0x6000 (24 KB) — sayaç, röle, eylem bayrakları, mail/telegram ayarları, WiFi |
| `otadata` | data/ota | 0xF000 | 0x2000 — rollback durumu |
| `phy_init` | data/phy | 0x11000 | 0x1000 |
| `ota_0` | app | 0x20000 | 0x1F0000 (1984 KB) |
| `ota_1` | app | 0x210000 | 0x1F0000 |

**`sdkconfig.defaults`** (ilgili satırlar):
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (`:17`): yeni görüntü `PENDING_VERIFY` ile açılır; `main.c` 60 sn sonra onaylar (§6.5). Onaylanmazsa bir sonraki açılışta önceki bölüme dönülür.
- Görev bekçisi: `CONFIG_ESP_TASK_WDT_INIT=y`, `_PANIC=y`, `_TIMEOUT_S=10`, `_CHECK_IDLE_TASK_CPU0=y` (`:38-41`); `CONFIG_ESP_INT_WDT=y`; `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y` (`:43`); `CONFIG_ESP_BROWNOUT_DET=y` (`:45`). Bekçiye yalnız boşta görevi kayıtlıdır; esp_timer görevi ya da mail görevleri kilitlenirse bekçi **devreye girmez** (boşta görev koşmaya devam eder).
- `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096` (`:47`).
- Konsol USB-Serial/JTAG (`:49-54`): UART0 TX/RX (GPIO16/17) harici flash SCLK/D7 ile çakışıyor.
- `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`:65`).
- Etkin değerler (`esp_bin/HW/sdkconfig`): `CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584` (`:1328`), `CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584` (`:1285`), `CONFIG_FREERTOS_HZ=100` (10 ms tik; `pdMS_TO_TICKS(50)`=5 tik), `CONFIG_FREERTOS_UNICORE=y`, CPU 160 MHz, `CONFIG_PM_ENABLE` kapalı.

---

## 6. `main/main.c` — uygulama birleştirici

### 6.1 Modül amacı

Bileşenleri sırayla başlatır, callback'leri bağlar ve asıl iş kurallarını taşır: erken uyarıda ve tetiklemede hangi kanalların (mail/Telegram/webhook+MQTT/röle) hangi NVS bayrağına göre çalışacağı, mail gruplarının nasıl okunup gönderileceği, OTA görüntüsünün ne zaman onaylanacağı, fabrika sıfırlamanın ne yaptığı. Dış arayüzü yoktur (`app_main` dışında her şey `static`).

### 6.2 İç işlevler (callback'ler ve görevler)

| İşlev | Satır | Bağlam | Ne yapar |
|---|---|---|---|
| `nvs_get_flag(ns,key,def)` | `:42-51` | çağıranın görevi | u8 oku, yoksa `def` |
| `ota_confirm_cb` / `ota_confirm_task` | `:69-74` / `:55-67` | esp_timer → görev `ota_ok` (4096, öncelik 3) | Çalışan bölüm `PENDING_VERIFY` ise `esp_ota_mark_app_valid_cancel_rollback()` + sys_log |
| `on_button_pressed` | `:79-100` | `btn_task` | `SL_EV_BUTTON`; **röle açıksa kapat** (`:85-87`); `timer_manager_restart()`; sys_log; SSE |
| `on_factory_reset` | `:103-112` | `btn_task` | `SL_RST_FACTORY`; `nvs_flash_erase()`; `ext_flash_format_user_data()`; 200 ms; `esp_restart()` |
| `alarm_mail_task` | `:117-165` | görev `alarm_mail` (10240, 5) | `ls_actions/early_mail` (vars. 0) → `smtp_client_has_config` → `ls_ew_mail` {email, subject, body} → tek `smtp_client_send` |
| `alarm_telegram_task` | `:168-202` | görev `alarm_tg` (8192, 5) | `ls_actions/early_tg` (vars. 0) → `telegram_client_has_config` → `cfg.message` ya da sabit metin (`:189`) → `telegram_client_send` |
| `api_notify_task` | `:205-210` | görev `alarm_api`/`trig_api` (8192, 5) | `api_client_notify_all(status)` ("alarm" / "triggered") |
| `on_alarm_triggered(idx)` | `:212-225` | **esp_timer görevi** | `get_state`, `sys_log_write`, üç görev yarat (api yalnız `early_api`=1 ise) |
| `trigger_mail_task` | `:230-339` | görev `trig_mail` (10240, 5) | `ls_actions/trig_mail` (vars. 0) → SMTP ayarı → `ls_tg_mail` grupları (bkz. 6.4) |
| `on_timer_expired` | `:342-364` | **esp_timer görevi** | `SL_EV_TIMER_EXPIRED`; `trig_relay` (vars. **1**); sys_log; röle; SSE; `trig_mail`; `trig_api` (yalnız `trig_api`=1 ise) |
| `generate_device_name` | `:370-396` | app_main | MAC → FNV-1a 64 → 10 karakter (taban 34; I ve O yok) → `LS-XXXXXXXXXX`, `device_name[20]` |
| `log_flash_info` | `:400-424` | app_main | açılış logu |

### 6.3 Erken uyarı zinciri ("alarm")

1. `timer_tick_cb` eşik geçişinde `s_alarm_cb(idx)` (`timer_manager.c:218`) → `on_alarm_triggered(idx)` esp_timer görevinde.
2. `sys_log_write("WARN","timer",…)` (`main.c:216`): heap tampon, `vsnprintf`, **mutex + LittleFS dosya ekleme** (`sys_log.c:600-631`, `append_line_locked` `:226-241`) — esp_timer görevinde senkron flash yazımı.
3. `xTaskCreate` ×2 (+1): `alarm_mail` (10240), `alarm_tg` (8192), `early_api` ise `alarm_api` (8192); öncelik 5; dönüş değerleri denetlenmez (`:218-223`).
4. Üç görev **eş zamanlı** koşar; mail ve Telegram kendi bayraklarını görev içinde okur, webhook/MQTT bayrağı dışarıda okunur. SMTP ve Telegram gönderimi çağıranın görevinde TLS açar (smtp_client/telegram_client kendi görev yaratmaz) → iki TLS oturumu aynı anda.
5. Her kanal sonucu `sys_log_write` ile kaydeder; **başarısızlıkta yeniden deneme yok**.
6. Erken uyarı maili ve Telegram metni tüm alarmlar için aynıdır (şablon/yer tutucu yok); alarm numarası yalnız loga yazılır. Webhook/MQTT şablonu `{status}{time}{device}` destekler (`api_client.h:8`).

### 6.4 Tetikleme zinciri ("trigger")

1. `timer_tick_cb`: `enabled=false, remaining=0`, `save_to_nvs()` (`timer_manager.c:222-224`) → `s_expiry_cb()` → `on_timer_expired()` esp_timer görevinde.
2. `sys_log_mark_event(SL_EV_TIMER_EXPIRED)` (ucuz, RTC bellek).
3. `trig_relay = nvs_get_flag("ls_actions","trig_relay",1)` (`:347`) — **varsayılan açık**, diğer tüm kanallar varsayılan kapalı.
4. `sys_log_write` (flash).
5. `trig_relay` ise `output_relay_trigger()` (`:351`): önce `stop_relay_worker` (önceki worker varsa esp_timer görevinde **≤1,5 sn `vTaskDelay`**), sonra `sys_log_mark_event` + `sys_log_write` (ikinci flash yazımı), `relay_wrk` görevi.
6. `web_server_send_timer_event()` (`:355`): yalnız `xTaskNotifyGive` (`web_server.c:1435-1440`), güvenli.
7. `xTaskCreate(trigger_mail_task, "trig_mail", 10240, …, 5)` (`:358`); `trig_api` ise `trig_api` görevi (`:361-363`).
8. `trigger_mail_task`: `ls_tg_mail/count` (u8), `i < count && i < 10` (`:259`); grup başına `g%d_subj` (64), `g%d_body` (256), `g%d_rcpt` (256, virgülle ayrılmış alıcılar); alıcı boşsa grup atlanır; `/ext/mail/g%d` dizinindeki en çok 5 düzenli dosya ek olarak toplanır (`MAX_ATTACH` `:276`, yol tamponu `malloc(300)`); her alıcıya **ayrı ve sıralı** `smtp_client_send[_with_attachments]`; başarı/başarısızlık sayılır, özet satırı yazılır (`:336-337`).
9. **Telegram tetiklemede gönderilmez** — `on_timer_expired` yalnız mail ve API görevlerini yaratır; `ls_actions` içinde `trig_tg` anahtarı yoktur (`web_server.c:1816-1819`).
10. Tetikleme **kalıcı değildir**: NVS'ye "tetiklendi, mail bekliyor" diye bir kayıt düşülmez. Mail gönderimi sırasında güç kesilir/çökerse, açılışta sayaç DURDU görünür ve hiçbir şey yinelenmez.

### 6.5 OTA onayı

`app_main` sonunda tek atımlık esp_timer `"ota_ok"` 60 sn (`OTA_VALID_AFTER_S` `:38`, `:502-510`). Dolunca `ota_confirm_cb` (esp_timer) → `ota_ok` görevi (flash yazımı esp_timer görevinde yapılmasın diye, `:72`) → bölüm `PENDING_VERIFY` ise onay. Zamanlayıcı yaratılamazsa hemen onay (`:508`). Ölçüt yalnız "60 sn çökmeden yaşamak"tır; WiFi/web/mail çalışıyor mu bakılmaz. `esp_timer_start_once` dönüşü denetlenmez (`:506`).

### 6.6 Erteleme yolları (kullanıcı check-in'i)

| Yol | Giriş | Röle | Log | Sonuç |
|---|---|---|---|---|
| Fiziksel düğme 1–2× | `btn_task` → `on_button_pressed` (`main.c:79`) | **kapatılır** (`:85-87`) | `SL_EV_BUTTON` + "Buton: geri sayım sıfırlandı" | `timer_manager_restart` |
| Web düğmesi | `POST /api/timer/set {"action":"restart"}` (`web_server.c:955-968`, kimlik doğrulamalı) | dokunmaz | "Geri sayım sıfırlandı (web)" | `timer_manager_restart` |
| Uzak API | `GET /api/reset?key=…` (`web_server.c:2012-2058`, kimlik doğrulamasız; `ls_reset/enabled` + sabit-zamanlı anahtar karşılaştırması) | dokunmaz | "Uzaktan sıfırlama API" | `timer_manager_restart` |

Üçünde de `total<=0` ise hata ("Önceden ayarlanmış süre yok"). Tatil sürerken üçü de "başarılı" döner ama sayaç tatil bitene kadar ilerlemez (§1.3).

### 6.7 Kalıcı veri — main.c'nin okuduğu NVS (başkası yazar)

| Namespace | Anahtar | Tip | Vars. | Yazan | Okuyan |
|---|---|---|---|---|---|
| `ls_actions` | `early_mail`, `early_tg`, `early_api`, `trig_mail`, `trig_api` | u8 | 0 | httpd `POST /api/actions/save` (`web_server.c:1800-1832`; GUI adları `early_telegram`, `trigger_mail`, …) | `main.c:122,173,222,235,361` |
| `ls_actions` | `trig_relay` | u8 | **1** | aynı | `main.c:347` |
| `ls_ew_mail` | `email`(64), `subject`(64), `body`(256) | str | boş | httpd `POST /api/early-mail/save` (`web_server.c:1495`) | `main.c:138-144` |
| `ls_tg_mail` | `count` u8; `g{i}_subj`(64), `g{i}_body`(256), `g{i}_rcpt`(256), i<10 | | 0 | httpd `POST /api/trigger-mail/save|delete` (`web_server.c:1591,1658`) | `main.c:249-271` |
| Dosya | `/ext/mail/g{i}/*` (harici flash LittleFS `user_data`) | | | httpd `POST /api/trigger-mail/upload` | `main.c:280-294` |

main.c kendi NVS'sine hiç yazmaz; `nvs_flash_erase()` yalnız fabrika sıfırlamada (`:108,436`).

### 6.8 Bağımlılıklar

`main.c` her bileşeni çağırır: `wifi_manager` (init, auto_connect), `web_server` (start, send_timer_event), `ext_flash` (init, create_default_files, format_user_data, slot/bilgi), `timer_manager`, `smtp_client` (init, has_config, send, send_with_attachments), `sys_log` (init, mark_event, write, set_restart_reason, boot_count, short_boots), `output`, `telegram_client` (init, has_config, get_config, send), `gui_ota`/`fw_ota` (init), `api_client` (init, notify_all), IDF: `nvs_flash`, `esp_ota_ops`, `esp_timer`, `esp_flash`, `esp_mac`, `esp_app_desc`.

---

## 7. `app_main` açılış sırası, görevler, uçtan uca akış

### 7.1 Adım adım (`main.c:426-528`)

| # | Satır | Adım | Hata davranışı |
|---|---|---|---|
| 1 | `:430` | `generate_device_name()` | — |
| 2 | `:433-439` | `nvs_flash_init()`; `NO_FREE_PAGES`/`NEW_VERSION_FOUND` ise **sil ve yeniden başlat** (tüm ayarlar gider) | `ESP_ERROR_CHECK` → abort |
| 3 | `:442-447` | `ext_flash_init()` | logla, devam |
| 4 | `:451-457` | `sys_log_init()` (önceki kapanışı analiz eder, `sys_log` görevi); ext flash tamamsa `ext_flash_create_default_files()` | — |
| 5 | `:460-464` | `timer_manager_init()` → **sayaç burada tiklemeye başlar**, callback'ler henüz bağlı değil | logla, devam |
| 6 | `:467-474` | `output_init()`; başarılıysa düğme/fabrika callback'leri | logla, devam (düğme ve röle yok) |
| 7 | `:475-477` | sayaç callback'leri: expiry → `on_timer_expired`, alarm → `on_alarm_triggered`, tick → `web_server_send_timer_event` | — |
| 8 | `:480-484` | `smtp_client_init`, `telegram_client_init`, `api_client_init`, `gui_ota_init`, `fw_ota_init` | yalnız uyarı |
| 9 | `:487-491` | `wifi_manager_init(device_name)` (AP modu) | logla, devam |
| 10 | `:494-498` | `web_server_start()` (httpd 8192 B yığın, öncelik 5; `sse_send` görevi) | logla, devam |
| 11 | `:502-510` | OTA onay zamanlayıcısı 60 sn | yaratılamazsa hemen onay |
| 12 | `:512` | `log_flash_info()` | — |
| 13 | `:514-524` | "Hazır" logu, `SL_EV_BOOT`, sys_log satırı (sürüm, slot, sayaç durumu) | — |
| 14 | `:527` | `wifi_manager_auto_connect()` (kayıtlı ağ varsa STA) | — |
| 15 | — | `app_main` döner; ana görev (3584 B) silinir | — |

Adım 5 ile 7 arasındaki pencere mikrosaniye düzeyindedir; ilk tik 1 sn sonra geldiğinden pratikte sorun çıkmaz, ama tasarım olarak callback kaydı init'ten önce olmalıydı.

### 7.2 Görev tablosu (bu alanın yarattığı ya da kullandığı)

| Görev | Yaratan | Yığın (B) | Öncelik | Ömür | İş |
|---|---|---|---|---|---|
| `main` | IDF | 3584 | 1 | açılış | `app_main` |
| `esp_timer` | IDF | 3584 | 22 | sürekli | 1 Hz sayaç tiki, alarm/expiry callback'leri, OTA onay cb |
| `btn_task` | `output_init` | 4096 | 10 | sürekli | çoklu basma, `on_button_pressed`/`on_factory_reset`/`esp_restart` |
| `relay_wrk` | `output_relay_trigger` | 3072 | 5 | geçici | gecikme / sürekli / darbe |
| `ota_ok` | `ota_confirm_cb` | 4096 | 3 | 60 sn sonra, kısa | görüntüyü onayla |
| `alarm_mail` | `on_alarm_triggered` | 10240 | 5 | geçici | erken uyarı maili (TLS) |
| `alarm_tg` | `on_alarm_triggered` | 8192 | 5 | geçici | erken uyarı Telegram (HTTPS) |
| `alarm_api` | `on_alarm_triggered` | 8192 | 5 | geçici | webhook + MQTT "alarm" |
| `trig_mail` | `on_timer_expired` | 10240 | 5 | geçici | grup mailleri + ekler (TLS) |
| `trig_api` | `on_timer_expired` | 8192 | 5 | geçici | webhook + MQTT "triggered" |
| `sse_send` | `web_server_start` | 3072 | 5 | sürekli | SSE push (`web_server.c:2638`) |
| `sys_log` | `sys_log_init` | 4096 | 1 | sürekli | 10 dk kontrol noktası (`sys_log.c:550`) |
| `httpd` | `web_server_start` | 8192 | 5 | sürekli | tüm API |

Mail/Telegram/API görevleri `smtp_client`/`telegram_client`/`api_client` içinde ayrı görev **yaratmaz**; TLS el sıkışması bu görevlerin yığınında yapılır. 10240 değeri projenin HTTPS yapan diğer görevi `gui_ota` ile aynıdır (`gui_ota.c:477`).

### 7.3 "Sayaç bitti → ne olur" uçtan uca

```
T-eşik_k   esp_timer: on_alarm_triggered(k)  →  sys_log(flash)  →  alarm_mail ‖ alarm_tg ‖ [alarm_api]
                                                                     (kendi bayraklarını okur, sonucu loglar, yeniden deneme yok)
T-0        esp_timer: enabled=0, remaining=0, NVS yaz
           on_timer_expired:
             SL_EV_TIMER_EXPIRED (RTC)
             trig_relay (vars. 1) → output_relay_trigger → [eski worker'ı bekle ≤1,5 s] → sys_log(flash) → relay_wrk
             SSE bildir (notify)
             trig_mail görevi → ls_tg_mail grupları → alıcı başına sıralı SMTP (+ ≤5 ek)
             [trig_api görevi → webhook, sonra MQTT]
T+0..       relay_wrk: delay → (sürekli: duration sn ya da sınırsız) | (darbe: pulse/pulse, duration sn ya da sınırsız)
Sonra      Sayaç DURDU; fiziksel düğme röleyi kapatır + restart; web/API yalnız restart.
           Yeniden başlatma: röle boşta; sayaç DURDU; hiçbir mesaj yinelenmez.
```

---

## 8. Gördüğüm sorunlar

Önem sırasına göre; her madde kodda doğrulandı.

1. **Tetikleme kalıcı değil, yeniden deneme yok** — `main.c:342-364`, `timer_manager.c:221-230`. Süre dolduğunda NVS'ye yalnız `enabled=0, remaining=0` yazılır. Mail görevleri sırasında güç kesilir, çöker ya da WiFi yoksa (STA bağlı değilken SMTP/Telegram zaten başarısız olur) protokol **sessizce kaybolur**; açılışta DURDU durumu "kullanıcı iptal etti" ile aynıdır. Güvenlik ürünü için en ağır eksik.

2. **Fiziksel düğme röleyi her durumda kapatmıyor** — `main.c:85-87` yalnız `output_relay_is_on()` doğruysa `output_relay_off()` çağırır. `s_relay_on` gecikme fazında (`output.c:157-160`) ve darbe modunun kapalı fazında (`output.c:220-221`) `false`'tur; bu anlarda basılan düğme worker'ı **durdurmaz**: röle gecikme bitince çeker ya da darbeye devam eder. Kullanıcı "ertele" demiş olsa da röle tetiklenir.

3. **Web/uzak API ertelemesi röleye dokunmuyor** — `web_server.c:955-968`, `web_server.c:2049-2057`. Yalnız fiziksel yol röleyi kapatır (`main.c:85`). `duration=0` (sürekli) ayarında web'den check-in yapan kullanıcı röleyi açık bırakır. Üç erteleme yolu aynı iş kuralını paylaşmıyor.

4. **Tatil sürerken `cancel` tatil sayacını donduruyor** — `timer_manager.c:380-390`: `stop_hw_timer()` ortak esp_timer'ı siler, ardından `ensure_hw_timer()` çağrılmaz. Tatil `enabled` ve `remaining>0` kalır ama tik yok; yeniden başlatmaya ya da başka bir `set/restart/vacation` çağrısına kadar tatil ilerlemez. GUI'de tatil "devam ediyor" görünür.

5. **`cancel_vacation` alarm sayacını sıfırlamıyor** — `timer_manager.c:447-467` `s_alarms_fired`/eşiği yeniden hesaplamaz; doğal tatil bitişi (`:173-177`) sıfırlar. Tatil öncesi 2 alarm atılmışsa, iptal sonrası tam süreden başlayan sayaçta yalnız son alarm gelir.

6. **esp_timer görevinde bloklayan iş** — `on_timer_expired` ve `on_alarm_triggered` esp_timer görevinde (öncelik 22, yığın 3584 B) `sys_log_write` (mutex `portMAX_DELAY` + LittleFS ekleme, `sys_log.c:624-628,226-241`), `nvs_open` (NVS kilidi, `main.c:347`), `save_to_nvs` (`timer_manager.c:224`) ve `stop_relay_worker` (≤1,5 sn `vTaskDelay`, `output.c:369-371`) çalıştırır. Bu sürede **tüm** esp_timer callback'leri (WiFi/LWIP iç zamanlayıcıları dahil) bekler. Başlık dosyasının "kısa tutun" sözleşmesi (`timer_manager.h:64,71,78`) çiğneniyor; `ota_confirm_cb` (`main.c:69-74`) doğru örüntüyü gösteriyor ama alarm/expiry buna uymuyor. Yığın: 3584 B içinde `vsnprintf` + VFS/LittleFS + NVS çağrı zinciri; taşma gözlenmedi ama pay dar.

7. **Alarm sayısı otomatik kırpması kalıcı ve birim tabanlı** — `timer_manager.c:348-354`. "1 gün" seçen kullanıcı 0 erken uyarıya düşer ve NVS'ye yazılır; sonra "30 gün" seçse de alarm sayısı kendiliğinden 3'e dönmez. Ölçüt `value/2` birimden bağımsız olduğundan "60 dakika" 10 alarma izin verirken "1 gün" 0'a izin verir — tutarsız.

8. **`xTaskCreate` dönüşleri hiç denetlenmiyor** — `main.c:73,218,220,223,358,362`, `output.c:300,390`. Heap tükenirse erken uyarı/tetikleme maili ya da röle **sessizce** atlanır; log bile yok.

9. **`inverted=true` bağlantıda açılış darbesi** — `output.c:254-274`: pin, `app_main`'in ilk dört adımı (NVS, harici flash, sys_log, sayaç) tamamlanana kadar sürülmez; boşta-enerjili röle bu pencerede "tetiklenmiş" görünür. Kod ele almıyor; donanım tarafında (pull-up) çözülmüş mü bilinmiyor.

10. **Süre dolunca esp_timer durmuyor** — `timer_manager.c:221-230` sonrası 1 Hz tik sonsuza kadar `:206`'da döner. Zararsız ama gereksiz uyanma; `ensure_hw_timer` çağrısı eksik.

11. **Kilitsiz paylaşılan durum** — `timer_manager.c:29-52`: `set_alarm_count` zamanlayıcıyı durdurmadan üç alanı yazar (`:479-482`); `get_state` tutarsız anlık görüntü verebilir (`:367-378`). Düşük olasılık, ama yeni tasarımda tek bir kritik bölge/mutex ya da "tüm değişiklikler esp_timer görevinde" kuralı gerekir.

12. **Tekrar eden kod** — `main.c`'de üç "bayrak oku → yapılandırma var mı → gönder → logla" görevi (`:117-165`, `:168-202`, `:230-339`) aynı iskeleti kopyalar; `nvs_get_flag` benzeri yardımcılar `web_server.c:1769-1799`'da yeniden yazılmış; `timer_manager.c` tik callback'inde "60 tikte NVS + 2 tikte SSE" bloğu iki kez (`:187-201`, `:232-244`).

13. **Sihirli sayılar** — `main.c:218-223,358,362` yığın boyutları ve öncelikler; `:259` `i < 10` grup sınırı ve `:276` `MAX_ATTACH 5` (web tarafındaki sınırlarla eşleşmesi tanım değil, rastlantı); `:137,261` tampon boyutları NVS'ye yazan tarafla (`web_server.c`) elle eşlenmiş; `output.c:86-87` 3 sn/400 ms; `:369` 30×50 ms; `timer_manager.c:24,53` 60/2.

14. **Düğme: tek basma 400 ms gecikir, çift basma = tek basma, ölü dal** — `output.c:87,99,133-137,114-117`. Kullanıcıya "çift basma" diye bir işlev sunulmuyor ama 2 basma da sayılıyor; `goto evaluate` dalı erişilemez ve yeni basmayı düşürür.

15. **Eski röle worker'ı yeni worker'ın durumunu bozuyor** — `output.c:364-376` ve `:243-250`. Worker gecikme fazındayken (`:157-160`, tek uzun `vTaskDelay`) `stop_relay_worker` 1,5 sn'de çıkmasını sağlayamaz, uyarı basıp tutamacı NULL'lar; `output_relay_trigger` hemen ikinci worker'ı yaratır. Eski worker gecikmesi bitince bildirimi görüp `done:` yoluna düşer ve **küresel** `s_relay_on`/`s_pulse_task_handle` üzerinde işlem yapar: yeni worker röleyi açmışsa onu kapatır (`:245-248`), her durumda yeni worker'ın tutamacını siler (`:250`). Sonuç: (a) tetiklenmiş röle beklenmedik anda söner, (b) yeni worker artık durdurulamaz — düğme/`output_relay_off` GPIO'yu çekse de worker darbe modunda röleyi yeniden açar. Senaryo somut: web "röle test" (gecikme >1,5 sn ayarlıyken) hemen ardından süre dolması.

16. **Grup mail gönderimi sıralı ve yavaş** — `main.c:296-328`: N alıcı × (TLS el sıkışma + ekleri yeniden okuma). 3 grup × birkaç alıcı × ek dosyalarla dakikalar sürer; bu sırada hata olursa kalan alıcılar beklemeye devam eder ama tamamlanma garantisi yok (bkz. 1).

17. **`system_info.flash_used_kb` sabit** — `system_info.c:39-52`: dahili bölümlerde `slot_a/slot_b/user_data` etiketi yok; toplam her zaman ≈4 028 KB. Yanıltıcı gösterge.

18. **`ESP_ERROR_CHECK(nvs_flash_init)` abort** — `main.c:439`: NVS bozuksa cihaz açılış döngüsüne girer; silme yalnız iki hata kodunda yapılır. Güvenlik ürünü için "ne olursa olsun aç, logla" tercih edilmeli.

19. **Alarm numarası logda +1, ESP_LOG'da 0 tabanlı** — `main.c:120` vs `:130`. Küçük tutarsızlık, hata ayıklamada kafa karıştırır.

20. **`ls_json_escape` kontrol karakterlerini sessizce atıyor** — `ls_util.c:97`; mail gövdesindeki sekme dışı kontrol karakterleri (ör. form feed) kaybolur. Önemsiz, ama belgelenmeli.

21. **`output` → `sys_log` bağımlılığı** — `output.c:128-129,385-387`: donanım sürücüsü loglama/yeniden başlatma politikası taşıyor; "3x = yeniden başlat" kararı sürücüde gömülü (`:126-131`), main.c'ye callback verilmiyor. Birim sınaması ve yeniden kullanım zorlaşıyor.

---

## 9. Korunması gereken davranışlar (kullanıcıya görünen, test edilebilir)

Sayaç:
- [ ] Süre `1..60` × {dakika, saat, gün} aralığında ayarlanabilir; dışı reddedilir.
- [ ] Süre ayarlanınca sayaç tam süreden, etkin olarak başlar ve erken uyarı sayacı sıfırlanır.
- [ ] "İptal" sayacı durdurur ve kalan süreyi 0 yapar; toplam süre korunur, "yeniden başlat" aynı süreyle tekrar başlatır.
- [ ] Hiç süre ayarlanmamışken "yeniden başlat" (düğme/web/API) hata döner: "Önceden ayarlanmış süre yok".
- [ ] Sayaç durumu NVS'de kalıcıdır: yeniden başlatma/güç kesintisi sonrası sayaç en çok ~60 sn fazlasıyla kaldığı yerden devam eder.
- [ ] Cihaz kapalıyken geçen süre sayılmaz (mevcut davranış; yeni tasarımda bilinçli karar gerekir — bkz. §10).
- [ ] Yeniden başlatma sonrası geçmişte kalmış erken uyarılar **yeniden gönderilmez** (açılışta hizalama).
- [ ] Süre dolduktan sonra yeniden başlatma tetiklemeyi yinelemez.

Erken uyarı:
- [ ] Alarm sayısı `0..10`; alarmlar kalan süre `total·(n−k)/(n+1)` eşiklerinde, sondan geriye eşit aralıklı (24 s/3 alarm → 18, 12, 6 s kala).
- [ ] Alarm sayısı çalışan sayaçta değiştirilince yeni eşikler hemen geçerli olur; geçmiş eşikler atlanır.
- [ ] Erken uyarı maili `ls_ew_mail` alıcısına (tek adres), Telegram `ls_telegram` chat_id'sine, webhook/MQTT `"alarm"` durumuyla gider; her kanal yalnız kendi `ls_actions` bayrağı açıksa çalışır; hepsi varsayılan kapalıdır.
- [ ] Eşzamanlı 3 kanal birbirini beklemez.

Tetikleme:
- [ ] Röle varsayılan olarak **açıktır** (`trig_relay` yoksa 1); mail/webhook/MQTT varsayılan kapalıdır.
- [ ] Tetiklemede Telegram gönderilmez (mevcut davranış; yeni tasarımda sorgulanmalı — §10).
- [ ] En çok 10 mail grubu; her grup: konu (≤63), gövde (≤255), virgülle ayrılmış alıcılar (≤255 karakter); alıcısı boş grup atlanır; her alıcıya ayrı mail.
- [ ] Grup ekleri `/ext/mail/g{i}/` altındaki dosyalardır, en çok 5 ek.
- [ ] Tetikleme sonrası sayaç DURDU durumuna geçer; toplam süre korunur.
- [ ] Webhook/MQTT `"triggered"` durumuyla, şablonda `{status}{time}{device}`.

Röle:
- [ ] Ayarlar: `duration` 0..3600 sn (0 = süresiz), `pulse` 0..60 sn (0 = sürekli), `delay` 0..60 sn, `inverted` bool; NVS'de kalıcı.
- [ ] Davranış: önce `delay` bekle; `pulse=0` → sürekli aç, `duration` sonra kapat (0 ise açık kal); `pulse>0` → `pulse` sn aç / `pulse` sn kapa, toplam `duration` (0 ise sonsuz).
- [ ] `inverted=true`: boşta enerjili, tetiklenince enerjisiz; ayar değişince röle kapalıysa seviye hemen uygulanır.
- [ ] Açılışta röle her zaman boşta seviyesindedir (röle durumu kalıcı değil).
- [ ] Web "röle test" tetiklemeyle aynı diziyi çalıştırır.
- [ ] Yeni tetik eskisini iptal eder (iki worker aynı anda sürmez).

Düğme:
- [ ] GPIO18 aktif LOW, pull-up, 200 ms debounce.
- [ ] 1 (ya da 2) basma → erteleme: röle kapanır, sayaç tam süreden başlar, SSE güncellenir, log "Buton: geri sayım sıfırlandı".
- [ ] 3 (ya da 4) basma 3 sn içinde → yeniden başlatma, log nedeni "buton 3x".
- [ ] 5 basma 3 sn içinde → fabrika sıfırlama: NVS tamamen silinir, harici kullanıcı verisi biçimlendirilir, yeniden başlar.
- [ ] Erteleme etkisi en geç ~400 ms içinde.

Sanal düğme:
- [ ] Web `POST /api/timer/set {"action":"restart"}` (oturum gerekli) ve `GET /api/reset?key=…` (oturum gerekmez; `ls_reset.enabled` ve anahtar eşleşmeli; yanlışta 401/403) sayacı tam süreden başlatır.

Tatil:
- [ ] `1..60` gün; başlayınca ana sayaç tam süreye çekilip dondurulur; tatil bitince ana sayaç kendiliğinden tam süreden, alarm sayacı sıfırlanmış olarak başlar.
- [ ] Tatil iptali ana sayacı hemen tam süreden başlatır.
- [ ] Tatil NVS'de kalıcıdır, yeniden başlatmada devam eder.

Sistem:
- [ ] Cihaz adı `LS-` + MAC'ten türetilen 10 karakter (I/O yok); AP SSID ve mDNS adı olarak kullanılır; her açılışta aynıdır.
- [ ] Yeni firmware 60 sn çökmeden çalışırsa onaylanır; aksi halde önceki sürüme dönülür.
- [ ] NVS sürüm uyuşmazlığı/sayfa doluluğunda NVS silinip devam edilir.
- [ ] Sayaç her 2 sn'de SSE ile tarayıcıya iletilir; düğme/iptal/yeniden başlat/tatil/süre dolma anında hemen iletilir.
- [ ] Olay günlüğüne yazılan satırlar (kategori `timer`/`mail`/`tg`/`api`/`relay`/`user`) ve düğme/tetik RTC olayları korunmalı (kapanış nedeni analizi bunlara dayanır).

---

## 10. Açık sorular

1. **Kapalıyken geçen süre**: Sayaç yalnız cihaz açıkken ilerliyor; haftalarca kapalı kalan cihaz açılınca kaldığı yerden sürüyor. Bu bilinçli bir "yanlış pozitif tetikleme üretme" kararı mı, yoksa RTC/SNTP yokluğundan mı? Yeni tasarımda SNTP varken duvar saatine göre düzeltme istenir mi?
2. **Telegram tetiklemede yok**: Erken uyarıda var, tetiklemede yok; GUI'de de `trigger_telegram` anahtarı yok. Kasıtlı mı (Telegram yalnız kullanıcının kendi hesabı olduğu için) yoksa unutulmuş mu?
3. **Tetikleme kalıcılığı**: Mail gönderimi yarıda kesilirse yeniden deneme beklenir mi? "Tetiklendi" durumu açılışta GUI'de gösterilmeli mi? (Şu an "iptal" ile "doldu" ayırt edilemiyor.)
4. **Alarm sayısı kırpması**: `value/2` kuralı istenen bir ürün kararı mı ("1 günlük sayaçta erken uyarı anlamsız") yoksa önceki bir hatayı kapatan yama mı?
5. **Erteleme = röle kapatma?** Fiziksel düğme röleyi kapatıyor, web/API kapatmıyor. Hangisi doğru davranış? Röle "sürekli" modunda kullanıcı check-in yapınca röle sönmeli mi?
6. **Röle gecikme fazında erteleme**: Gecikme sırasında düğmeye basılırsa röle çekmemeli mi? (Şu an çekiyor.)
7. **`inverted=true` açılış darbesi**: Devrede pull-up ile çözülmüş mü, yoksa yazılımda erken GPIO ayarı gerekli mi?
8. **Düğme çift basma**: 2 basma için ayrı bir işlev planlanmış mıydı (yorum `output.c:133` "1x veya 2x")?
9. **OTA onay ölçütü**: 60 sn çökmeme yeterli mi, yoksa "WiFi bağlandı + web yanıt verdi" gibi işlevsel bir ölçüt mü istenir?
10. **Mail gönderim sırası/süresi**: Grup içi alıcılara ayrı ayrı gönderim (BCC yerine) bilinçli mi (alıcıların birbirini görmemesi için)? Ekler her alıcı için yeniden okunuyor; kabul edilebilir mi?
11. **NVS yazım sıklığı**: Çalışırken her 60 sn 7 anahtar + commit. 24 KB NVS ile ömür hesaplanmış mı? (IDF NVS değişmeyen değeri yeniden yazmasa bile `t_remain` her dakika değişir.)
12. **`early_api`/`trig_api` bayrakları dışarıda, `early_mail`/`trig_mail` görev içinde okunuyor**: tutarlılık için hepsinin tek yerde okunması tercih edilir mi?
