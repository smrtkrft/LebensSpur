# 05 — WiFi araştırması (`net_wifi`)

Plan: `02-plan-ilk.md` §5.4 ve §8 soru 8. Eski uygulama: `ALTE/HW/components/wifi_manager/wifi_manager.c` (harita: `01-harita/02-ag-ve-iletisim.md` §1). Hedef: ESP32-C6, ESP-IDF 5.5.2.

Kaynak kısaltmaları (hepsi `~/esp/esp-idf-v5.5.2/` altında):

| Kısaltma | Dosya |
|---|---|
| `W` | `components/esp_wifi/include/esp_wifi.h` |
| `T` | `components/esp_wifi/include/esp_wifi_types_generic.h` |
| `TN` | `components/esp_wifi/include/local/esp_wifi_types_native.h` |
| `K` | `components/esp_wifi/Kconfig` |
| `D` | `docs/en/api-guides/wifi.rst` |
| `S` | `docs/en/api-guides/wifi-security.rst` |
| `N` / `NL` | `components/esp_netif/include/esp_netif.h` / `components/esp_netif/lwip/esp_netif_lwip.c` |
| `NS` | `components/esp_netif/include/esp_netif_sntp.h`, `components/esp_netif/lwip/esp_netif_sntp.c` |
| `LK` | `components/lwip/Kconfig` |
| `PK` / `PI` | `components/esp_phy/Kconfig` / `components/esp_phy/src/phy_init.c` |
| `A` | `ALTE/HW/components/wifi_manager/wifi_manager.c` |
| `M` | `esp_bin/HW/managed_components/espressif__mdns/` (sürüm 1.14.0, `esp_bin/HW/dependencies.lock:12`) |

"Koddan çıkarım" / "ölçülecek" diye işaretlenenler kanıtlanmış değildir; adım 3'te tezgâhta doğrulanacak.

---

## 0. Özet — tasarım kararının doğrulanması

Plan §5.4'teki karar (olay döngüsünde tek durum makinesi, kendi görevi yok, async tarama, esp_timer'lı yeniden deneme, APSTA, 10 dk AP otomatik, 30 dk birincil denemesi, hostname, `STA_LOST_IP`, TX gücü aynen, `ESP_WIFI_NVS_ENABLED=n`) **IDF belgesiyle uyumlu ve uygulanabilir**. Beş düzeltme/iyileştirme gerekiyor:

1. **"reason 2/8 = biz kopardık" varsayımı yanlış.** IDF'de `WIFI_REASON_AUTH_EXPIRE = 2` (kimlik doğrulama zaman aşımı → gerçek hata), `WIFI_REASON_AUTH_LEAVE = 3` (yalnız **AP'den gelir**, örn. router yeniden başlarken deauth), `ASSOC_LEAVE = 8` hem AP'den hem `esp_wifi_disconnect()`'ten (`T:115-124`, `D:904-974`). ALTE `AUTH_LEAVE`(3) ve `ASSOC_LEAVE`(8) için yeniden denemeyi atlıyor (`A:470-472`); harita ise "2" yazıyor. Router'ın yolladığı 3 ya da 8 ile cihaz **sonsuza dek bağlanmamayı** seçebilir. Çözüm: reason'a değil, kendi `self_disconnect` bayrağımıza bakmak (§2).
2. **Tarama süresi AP istemcileri için bütçelenmeli.** APSTA'da tarama "arka plan" taramasıdır: her kanaldan sonra 30 ms (`home_chan_dwell_time`, 30–150 ms) ana kanala dönülür (`D:656-658,680-700`, `W:541-543`). Varsayılan 120 ms/kanal × 13 kanal ≈ 1,95 s; ALTE'nin `max=500` ayarı en kötü ≈ 6,9 s (AP yalnız %6 kanalda). Öneri: 120 ms + istemci varken `home_chan_dwell_time=100` ve tarama aralığını istemci sayısına göre uzatmak (§1).
3. **Bağlantı sonrası tek kanal taraması.** Taramadan öğrenilen kanalı `sta.channel` ipucu + `WIFI_FAST_SCAN` ile verince sürücünün bağlanma öncesi kendi taraması ~1 kanala iner (`D:1397-1399`, `T:349`). ALTE her bağlanışta 13 kanal daha tarıyor (`A:272-273`).
4. **`esp_wifi_disconnect` → 200 ms `vTaskDelay`** gereksiz ve olay görevini bloklar; doğru kalıp `STA_DISCONNECTED` olayını bekleyip ondan sonra `set_config + connect` (`DISCONNECTING` durumu, §3).
5. **TX gücü GUI'de 9 nicemlenmiş kademe** (7 / 8,5 / 11 / 13 / 14 / 15 / 16,5 / 18 / 20 dBm) olarak gösterilmeli; config kayıt değeri 0,25 dBm biriminde tutulmalı (`W:1121-1124`, §4). ALTE'nin "8 dBm"i fiilen 7, "17"si 16,5 dBm.

Ek bulgular: `IP_EVENT_STA_LOST_IP` kopmadan **120 s** sonra gelir (`esp_netif/Kconfig:3-13`), kira yenileme hatasında da; statik IP'de gelmeyebilir. Modem uykusu **yalnız STA-only modda** çalışır (`D:2160`), APSTA'da `WIFI_PS_NONE` çağrısı etkisiz ama zararsız. DHCP sunucusu varsayılan olarak **AP'nin kendi IP'sini DNS diye sunar** (`LK:428-434`, `dhcpserver.c:480-484`) ama cihazda DNS sunucusu yok → `.local` (mDNS) dışında ad çözümü yok. `CONFIG_ESP_PHY_REDUCE_TX_POWER` brownout sonrası açılışta PHY tavanını 13 dBm'e çeker (`PI:844-893`, `esp_phy/esp32c6/include/phy_init_data.h:21`) — "yumuşak başlangıç"ın IDF'deki tek akrabası.

---

## 1. Async tarama APSTA'da

### Bulgu

**Akış.** `esp_wifi_scan_start(&cfg, false)` hemen döner; bitince `WIFI_EVENT_SCAN_DONE` gelir (`wifi_event_sta_scan_done_t {status 0/1, number, scan_id}`, `T:1135-1139`). Olay şu durumlarda da gelir: `esp_wifi_scan_stop()` çağrılınca ve tarama bitmeden yeni `scan_start` yapılınca (yenisi eskisini geçersiz kılar) (`D:244-248`). Gelmez: engelleyici taramada ve `esp_wifi_connect()`'in kendi taramasında (`D:250-253`). Olay görevinin kendisi hiçbir şey yapmaz; **uygulama `esp_wifi_scan_get_ap_num` + `esp_wifi_scan_get_ap_records` (ya da `esp_wifi_clear_ap_list`) çağırmak zorundadır, yoksa sürücü belleği sızar** (`D:255`, `W:504-506`). `get_ap_records` listeyi RSSI'ye göre azalan sıralı verir ve **tek seferde** tüm belleği boşaltır; iki kez çağırmayın (`W:597-600`, `D:678`). Alternatif: `esp_wifi_scan_get_ap_record(&r)` döngüsü kayıtları **tek tek** verir ve her birini serbest bırakır; hepsini almazsanız `esp_wifi_clear_ap_list()` şart (`W:615-629,644`). `wifi_ap_record_t` 26 alanlı (~90 B); 20 kayıt ≈ 1,8 KB heap (ALTE `malloc`, `A:327-328`) — tek tek okuma ile sıfır heap.

**Kanal dışı süre.** Dwell kuralları (`D:611-616`): `min=0,max=0` → 120 ms/kanal; `min=0,max>0` → `max` ms; `min>0,max>0` → kanalda AP yoksa `min` sonra geç, varsa `max` kadar kal. Pasif 360 ms/kanal (`T:190`). Üst sınır 1500 ms (`W:507-508`). Varsayılan `home_chan_dwell_time` 30 ms, izin verilen 30–150 ms (`W:538-543`, `T:191`); `wifi_scan_config_t.home_chan_dwell_time` alanı 5.5.2'de **var** (`T:245`), ayrıca `esp_wifi_set_scan_parameters()` ile kalıcı varsayılan yapılabilir ve **bu değerler `esp_wifi_connect`'in kendi taramasında da kullanılır** (`W:527-554`). Hesap (13 kanal, EU/TR):

| Yapılandırma | Tarama süresi | AP'nin kendi kanalında kaldığı pay |
|---|---|---|
| IDF varsayılanı (0/120, dwell 30) | 13×(120+30) ≈ **1,95 s** | 13×30 = 0,39 s (%20) |
| 0/120, dwell 100 | 13×(120+100) ≈ 2,9 s | 1,3 s (%45) |
| ALTE (120/500, dwell 30) (`A:309-314`) | en kötü 13×(500+30) ≈ **6,9 s** | 0,39 s (%6) |
| Pasif 360 | 13×390 ≈ 5,1 s | 0,39 s (%8) |

"Arka plan" taraması belgede "STA bağlıyken **ya da STA/AP modunda**" diye tanımlı (`D:567-568`); ana kanala dönüşün amacı "bağlantıya veri alıp verme fırsatı tanımak" (`D:656-658`). AP beacon'larının tam olarak bu 30 ms'de çıkıp çıkmadığı belgede **yazmıyor** (sürücü kapalı kaynak). Forum (esp32.com "esp_wifi_connect() while running in WIFI_MODE_APSTA causes access point to close", p=5264/5285/8462/14119/29910; "WiFi APSTA keeps disconnecting clients", p=114512): STA tarama/bağlanma sırasında tek radyo tek kanalda olduğundan AP istemcileri **kopabilir**; ayrıca STA farklı kanaldaki router'a bağlanınca AP, CSA ile kanal değiştirir (`csa_count` varsayılan 3 beacon, `T:533`) ve istemciler yeniden eşitlenir. Sonuç: tarama ≤2 s tutulursa ve ardışık taramalar seyrekse telefonlar genelde düşmez (telefon beacon-kaybı toleransı birkaç saniyedir; **ölçülecek**).

**Çakışmalar.** STA bağlanırken `esp_wifi_scan_start` → `ESP_ERR_WIFI_STATE` (`W:522`, `D:736-752`: bağlanma taramadan önceliklidir; sonsuz reconnect döngüsü taramayı "aç bırakır", bu yüzden sayaçlı/aralıklı yeniden bağlanma şart). Uygulama taraması sürerken `esp_wifi_connect` çağrılırsa tarama iptal edilir ve `ESP_ERR_WIFI_STATE` döner (`W:448-450`). `esp_wifi_set_config` bağlanırken `ESP_ERR_WIFI_STATE` (`W:1002`). Eşzamanlı iki tarama desteklenmez (`D:731-734`). Bütün bunlar tek durum makinesiyle (SCANNING ↔ CONNECTING asla aynı anda değil) sıfır maliyetle çözülür.

**Tarama mı, doğrudan `esp_wifi_connect` mi?** `esp_wifi_connect` kendi başına "belirli AP için tüm kanal" taraması yapar; `WIFI_FAST_SCAN` ise hedef bulununca durur, `channel` ipucu verilirse o kanaldan başlar (`D:758-790,1397-1399`; `T:349-351,557`). Router menzildeyse FAST_SCAN + kanal ipucu ≈ **1 kanal (~150 ms)**; router yoksa yine 13 kanal (~1,6 s) ve `NO_AP_FOUND`(201). Açık tarama ise tek geçişte **hem birincil hem yedeğin** varlığını, RSSI'sini ve kanalını verir. ALTE her iki ağ görünürken RSSI karşılaştırmaz (`A:348-355`).

### Öneri

- Tarama yapılandırması: `scan_type=ACTIVE`, `scan_time.active={0,120}`, `show_hidden=false` (gizli BSS SSID'siz geldiği için eşleşmeye yaramaz; `D:603`), `home_chan_dwell_time = ap_clients ? 100 : 30` (**ölçülecek**: telefon bağlıyken 120/30 ile 120/100 arasında `AP_STADISCONNECTED` sayısı).
- Sonuçları `esp_wifi_scan_get_ap_record` döngüsüyle oku (heap yok), bitince `esp_wifi_clear_ap_list()` (idempotent güvenlik).
- Seçim: birincil görünüyorsa birincil (plan kararı; RSSI karşılaştırması isteniyorsa `p_rssi < b_rssi - 15` gibi tek eşik). Taramada bulunan kanalı `sta.channel`'a yaz, `scan_method=WIFI_FAST_SCAN` → bağlanma öncesi tarama ~1 kanal. Kanal ipucu yoksa `WIFI_ALL_CHANNEL_SCAN` + `BY_SIGNAL`.
- "Gizli ağ olabilir, kör dene" (`A:357-364`) yerine config'e **`hidden` bayrağı** (GUI onay kutusu). Bayrak yoksa taramada görünmeyen ağa connect denenmez → her döngüde ~1,6 s ek kanal dışı süre biter.
- Tarama aralığı (STA kayıpken): ilk 2 dk 15 s; sonra 60 s; **AP istemcisi varken en az 60 s** (kullanıcı GUI'de; "Bağlan" düğmesi zaten anında dener). İstemci yokken kanal dışı sürenin maliyeti yoktur, 15 s serbest.
- Yedekteyken 30 dk'lık birincil denemesi: bağlantıyı koparmadan **arka plan taraması** yap; birincil görünür ve RSSI ≥ −75 ise `DISCONNECTING(hedef=birincil)`; görünmüyorsa bağlı kal (plan kararı 17 iyileştirmesi). Bağlı STA'da ~2 s tarama paket kaybı yaratır ama kopma yok.
- Bir GUI "Bağlan" isteği SCANNING sırasında gelirse `pending` bayrağı; `SCAN_DONE`'da işlenir (isteğe bağlı `esp_wifi_scan_stop()` ile erken bitirme — yine `SCAN_DONE` gelir).

### Risk

- AP beacon davranışı taramada belgelenmemiş; 100 ms dwell faydası ölçümle doğrulanmalı.
- `esp_wifi_scan_get_ap_record` döngüsü bitmeden başka bir tarama başlatılırsa kayıtlar karışır — olay görevinde tek parça işlendiği için durum makinesi bunu önler.
- Ülke kodu ayarlanmazsa kanal sayısı sürücü varsayılanına bağlı; `esp_wifi_set_country_code("TR", true)` ile 13 kanal sabitlenir (`W:1562`).

---

## 2. Bağlantı hata nedenleri

### Bulgu

`WIFI_EVENT_STA_DISCONNECTED` yükü: `{ssid, ssid_len, bssid, reason (uint8), rssi (int8)}` (`T:1156-1162`). Reason tablosu `D:879-1270`; parola ile ilgili olanlar ayrıca listelenmiş (`D:1228-1252`): **15** (4WAY_HANDSHAKE_TIMEOUT: şifreli AP'ye yanlış parola), **204** (HANDSHAKE_TIMEOUT: aynı), **201** (NO_AP_FOUND: açık AP'ye parola verildi **ya da** şifreli AP'ye parola verilmedi). Düşük RSSI ile ilgili: 212, 204.

5.5.2'de dikkat çeken kodlar (`T:113-178`, `D`):

| Kod | Ad | Ne zaman (STA tarafı) |
|---|---|---|
| 2 | AUTH_EXPIRE | auth zaman aşımı **veya** AP'den geldi (`D:904-917`) |
| 3 | AUTH_LEAVE | **yalnız AP'den** gelir (router kapanıyor/yeniden başlıyor) (`D:918-924`) |
| 4 | DISASSOC_DUE_TO_INACTIVITY | assoc zaman aşımı veya AP'den |
| 5 | ASSOC_TOOMANY | AP dolu |
| 8 | ASSOC_LEAVE | AP'den **veya** `esp_wifi_disconnect()` ve diğer API'ler (`D:967-974`) |
| 15 / 204 | 4-yollu el sıkışma zaman aşımı | yanlış parola (WPA2) |
| 200 | BEACON_TIMEOUT | N beacon kaçtı → menzil dışı / router kapandı (varsayılan 6 s + 5 probe, `W:1355-1357`, `D:1290-1297`) |
| 201 | NO_AP_FOUND | taramada hedef yok (parola/açık uyumsuzluğu dahil) |
| 202 | AUTH_FAIL | kimlik doğrulama reddi (zaman aşımı dışı; WPA3/SAE'de yanlış parola bu sınıfa düşer — **koddan çıkarım**) |
| 203 | ASSOC_FAIL | ilişkilendirme reddi |
| 205 | CONNECTION_FAIL | genel |
| 207 | ROAMING | yalnız 11kv/FT açıkken; bilgi amaçlı, STA kendisi taşınır |
| 210 | NO_AP_FOUND_W_COMPATIBLE_SECURITY | güvenlik uyumsuz (WEP parola, PMF, SAE H2E/HnP, OWE…) |
| 211 | NO_AP_FOUND_IN_AUTHMODE_THRESHOLD | eşik WPA2 iken router WPA/WEP |
| 212 | NO_AP_FOUND_IN_RSSI_THRESHOLD | eşik altı sinyal |

Birden çok BSS aynı SSID'deyse rapor edilen reason "başarıya en yakın" olandır; `NO_AP_FOUND_*` kodları önem sırasına göre seçilir (`D:884`).

**`IP_EVENT_STA_LOST_IP`:** kopma anında değil, "IP kaybı zamanlayıcısı" dolunca gelir; süre `CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL` = **120 s** (0 = kapalı) (`esp_netif/Kconfig:3-13`, `D:331-337`). Zamanlayıcı şu noktalarda başlar: netif düşünce (kopma), DHCP istemcisi durdurulunca, DHCP kira yenileme başarısız olup adres sıfırlanınca (`NL:1409-1536,1555-1566,1620-1627,1878`). Belge "genelde yok sayılabilir, hata ayıklama olayı" der (`D:337`). Statik IP'de "son yapılandırma `ip_info`'da tutulur" (`NL:1473-1476`) → olay gelmeyebilir. Ayrıca `IP_EVENT_STA_GOT_IP` adres **değişince** de gelir (`ip_change=true`; kira yenilenemeyip 0'a dönmesi dahil, `D:305-320`).

ALTE: `STA_LOST_IP` kaydedilmiyor (`A:585-586`); `s_ip_addr` kopmada sıfırlanmıyor (harita bulgu 7).

### Öneri

**Kendi kopmamızı reason ile değil bayrakla tanı:** `esp_wifi_disconnect()`'ten hemen önce `self_disc = true`; `STA_DISCONNECTED`'da bayrak doluysa "bizim" (reason ne olursa olsun), değilse "dış" — dış 3/8'de **yeniden dene**. Durum makinesinde bu `DISCONNECTING` durumuna karşılık gelir.

GUI için reason → üç sınıf + ayrıntı (API'de hem `reason` sayısı hem sınıf):

| Sınıf (`/api/wifi.last_error`) | Reason'lar | GUI metni |
|---|---|---|
| `auth` | 15, 204, 202, 23, 14; **201 ve parola boşsa** | "Parola yanlış ya da ağ parola istiyor" |
| `not_found` | 201 (parola doluysa), 212, 200, 2, 4 | "Ağ bulunamadı / menzil dışı" |
| `security` | 210, 211, 18, 19, 20, 24 | "Güvenlik uyumsuz (WPA/WEP ya da PMF/WPA3 ayarı)" |
| `other` | 1, 5, 203, 205, diğer | "Router bağlantıyı reddetti (kod N)" |

Sınıf, **5 denemenin sonundaki** son reason'dan türetilir (ilk deneme 201 gelip ikincide 15 gelebilir; en sonuncusu doğrudur). `rssi` alanı da evlog'a yazılır (ALTE'deki gibi).

`LOST_IP`: `CONNECTED` iken gelirse (kira yenilenemedi) → `self_disc` + `esp_wifi_disconnect()` → `DISCONNECTING(hedef=aynı)` → temiz DHCP ile yeniden bağlan. `GOT_IP` yükünde `ip_info.ip.addr == 0` ise aynı yol. Kopmada IP metni sıfırlanır (plan).

### Risk

- `AUTH_EXPIRE`(2) hem menzil hem router sorununu kapsar; GUI'de "bulunamadı" sınıfında kalması makul ama yanıltabilir — evlog'a ham kod yazıldığı için izlenebilir.
- WPA3 yanlış parola gerçekten 202 mi veriyor, tezgâhta doğrulanmalı (router WPA3-only testi).

---

## 3. Durum makinesi olay görevinde

### Bulgu

- Varsayılan olay döngüsü görevi: öncelik `ESP_TASKD_EVENT_PRIO = configMAX_PRIORITIES − 5 = 20` (`esp_system/include/esp_task.h:48`, `freertos/config/include/freertos/FreeRTOSConfig.h:93`); yığın `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` (varsayılan 2304, ALTE 4096 `sdkconfig.defaults:47`) + `TASK_EXTRA_STACK_SIZE` (+2048 yalnız `LWIP_TCPIP_CORE_LOCKING` açıksa) (`esp_task.h:49-53`); kuyruk 32 (`esp_system/Kconfig:213-223`). lwIP görevi 18 (`LK:21-24`). Olay görevi httpd ve bizim işçi görevlerimizden yüksek öncelikli; uzun handler tüm sistemin olaylarını geciktirir.
- WiFi sürücüsü olayları `esp_event_post` ile yollar; bloklayan çağrı istenince `portMAX_DELAY` (`esp_wifi/esp32c6/esp_adapter.c:276-281`). Kuyruk dolarsa (32) WiFi görevi bekler → handler içinde uzun bloklama **WiFi görevini de durdurur**. Handler'da flash yazımı (ALTE `sys_log_write`, `A:439,477,502,533`) bu yüzden kötüydü; 2611 evlog halkası bunu çözer.
- Handler içinden `esp_wifi_connect/disconnect/scan_start/set_config/set_mode` çağırmak **IDF'in resmî kalıbı**: "application event callback needs to call esp_wifi_connect()" (`D:261,287,457,481`). Bütün `esp_wifi_*` çağrıları WiFi görevine senkron RPC'dir (ms mertebesi; süre belgelenmemiş). `esp_wifi_scan_start(…, false)` hemen döner. `esp_netif_*` çağrıları lwIP görevinde koşar ve biter (`NL:1783` `_RUN_IN_LWIP_TASK_IF_SUPPORTED`), olay görevinden güvenli.
- `esp_wifi_set_mode`: APSTA→STA AP'yi **durdurur** (istemciler AP tarafından `AUTH_EXPIRE`(2) ile düşürülür: "AP is stopped", `D:904-917`; `WIFI_EVENT_AP_STOP`). STA→APSTA AP'yi başlatır (`AP_START`); STA bağlıysa AP onun kanalında açılır (`W:982-983`). AP→APSTA'nın mevcut AP'yi yeniden başlatıp başlatmadığı **belgelenmemiş** (forum: mod geçişi değil, ardından gelen STA tarama/bağlanma istemcileri düşürür). ALTE'de bu geçiş açılışta AP istemcisi yokken yapılır (`A:236-240,686`); 2611'de aynı.
- `esp_wifi_disconnect` sonrası `esp_wifi_connect`: belge "bağlıysan önce disconnect" der (`W:447`), bekleme süresi belirtmez; `disconnect` kesin olarak `STA_DISCONNECTED`(8) üretir (`D:967-974`). ALTE'nin 200 ms'i (`A:282`) olayı beklememenin bedelidir. Bağlanırken (CONNECTING) `set_config` `ESP_ERR_WIFI_STATE` döner (`W:1002`); `disconnect` bağlanmayı da iptal edip `STA_DISCONNECTED` üretir → her yeni hedef için tek yol: `disconnect → olay → set_config → connect`. STA boşta ise (hiç bağlanmamış/kopmuş) disconnect gereksiz, doğrudan `set_config + connect`.
- `wifi_sta_config_t.failure_retry_cnt` sürücü düzeyinde "aynı AP'yi n kez dene" (yalnız ALL_CHANNEL_SCAN, `T:573-574`); zamanlı geri çekilme sağlamaz → kendi 1..5 s zamanlayıcımız kalır, `failure_retry_cnt=0`.

### Öneri — durum makinesi

Durumlar: `AP_ONLY` (kayıt yok ya da kullanıcı unutturdu), `SCANNING`, `CONNECTING` (deneme k/5; aralar `t_retry` ile), `CONNECTED`, `DISCONNECTING` (bizim kopardığımız, hedef bekliyor), `RETRY_WAIT` (uzun bekleme; EXHAUSTED karar anı bu duruma girişte işlenir). Olaylar: `SCAN_DONE`, `STA_CONNECTED`, `GOT_IP`, `DISC` (dış), `DISC_SELF`, `LOST_IP`, `t_retry`, `t_rescan`, `t_ap_auto` (10 dk), `t_primary` (30 dk), `U_CONNECT`, `U_FORGET`, `U_AP_ON/OFF`.

| Durum | Olay | Eylem | Yeni durum |
|---|---|---|---|
| AP_ONLY | kayıt var (açılış) / U_CONNECT | APSTA değilse `set_mode(APSTA)`; `scan_start(async)` | SCANNING |
| AP_ONLY | U_AP_OFF | reddet (`ESP_ERR_INVALID_STATE`) | AP_ONLY |
| SCANNING | SCAN_DONE: birincil bulundu | hedef=p, kanal ipucu; `set_config`; `connect`; k=1 | CONNECTING |
| SCANNING | SCAN_DONE: yalnız yedek | hedef=b (yedek bayrağı) | CONNECTING |
| SCANNING | SCAN_DONE: hiçbiri, `p.hidden` | hedef=p, ipucu yok (ALL_CHANNEL) | CONNECTING |
| SCANNING | SCAN_DONE: hiçbiri | evlog (bir kez); `t_rescan`; AP kapalıysa `t_ap_auto` | RETRY_WAIT |
| SCANNING | U_CONNECT | `pending=1` (`scan_stop` isteğe bağlı) | SCANNING |
| CONNECTING | STA_CONNECTED | kanalı sakla | CONNECTING |
| CONNECTING | GOT_IP (ip≠0) | k=0; IP metni; `set_ps`; SNTP (tek sefer); `t_ap_auto` durdur; yedekteyse `t_primary` başlat; evlog | CONNECTED |
| CONNECTING | DISC, k<5 | `last_reason`; k++; `t_retry(1 s×k)` | CONNECTING |
| CONNECTING | t_retry | `esp_wifi_connect()` | CONNECTING |
| CONNECTING | DISC, k=5, hedef=p ve yedek var | hedef=b; `set_config`; `connect`; k=1 | CONNECTING |
| CONNECTING | DISC, k=5 (diğer) | hata sınıfı; evlog (bir kez); `t_rescan`; AP kapalıysa `t_ap_auto` | RETRY_WAIT |
| CONNECTING | U_CONNECT / U_FORGET(aktif) | `self_disc=1`; `disconnect` | DISCONNECTING |
| CONNECTED | DISC | IP=0; evlog(reason, rssi); k=1; `t_retry(1 s)` (tarama yok, aynı hedef) | CONNECTING |
| CONNECTED | LOST_IP / GOT_IP ip=0 | `self_disc=1`; `disconnect`; hedef=aynı | DISCONNECTING |
| CONNECTED | t_primary (yedekteyken) | `scan_start(async)` (arka plan) | CONNECTED |
| CONNECTED | SCAN_DONE: p görünür, RSSI≥−75 | `self_disc=1`; `disconnect`; hedef=p | DISCONNECTING |
| CONNECTED | SCAN_DONE: diğer | `t_primary` yeniden | CONNECTED |
| CONNECTED | U_CONNECT | `self_disc=1`; `disconnect`; hedef=yeni | DISCONNECTING |
| CONNECTED | U_AP_OFF | `set_mode(STA)`; `ap_enabled=0` (cfg) | CONNECTED |
| CONNECTED | U_AP_ON | `set_mode(APSTA)` + AP cfg | CONNECTED |
| DISCONNECTING | DISC_SELF (her reason) / 2 s koruma zamanlayıcısı | hedef varsa `set_config`; `connect`; k=1 → CONNECTING; yoksa AP'yi zorla aç | CONNECTING / AP_ONLY |
| RETRY_WAIT | t_rescan | `scan_start` | SCANNING |
| RETRY_WAIT | U_CONNECT | hemen tara | SCANNING |
| RETRY_WAIT | t_ap_auto | `set_mode(APSTA)` + AP cfg; evlog "AP otomatik açıldı" | RETRY_WAIT |
| her durum | AP_STACONNECTED/DISCONNECTED | istemci sayısı; TX kick; tarama aralığı güncelle | aynı |
| her durum | bayat DISC/SCAN_DONE (beklenmeyen) | yoksay, log | aynı |

`t_rescan` aralığı: `ap_clients>0 ? 60 s : (kayıptan beri <2 dk ? 15 s : 60 s)`. `U_FORGET` aktif ağı siliyorsa AP kapalı olamaz (zorla aç) — aksi hâlde cihaz erişilemez kalır.

### İskelet (olay görevinde, ≤40 satır)

```c
typedef enum { ST_AP_ONLY, ST_SCANNING, ST_CONNECTING, ST_CONNECTED, ST_DISCONNECTING, ST_RETRY_WAIT } st_t;
static struct { st_t st; uint8_t k; bool self_disc, backup, pending; uint8_t reason; uint8_t chan; } s;

static void start_connect(const net_cfg_t *n, uint8_t ch, bool backup) {
    wifi_config_t c = {0};
    strlcpy((char *)c.sta.ssid, n->ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, n->pass, sizeof c.sta.password);
    c.sta.channel = ch;                                   /* taramadan gelen kanal ipucu */
    c.sta.scan_method = ch ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    c.sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.sta.threshold.rssi = -95;
    c.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;               /* H2E-only router'lar için */
    ESP_RETURN_VOID_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &c));  /* ESP_ERR_WIFI_STATE → log */
    apply_static_ip(&n->st);
    s.k = 1; s.backup = backup; s.st = ST_CONNECTING;
    esp_wifi_connect();
}
static void on_scan_done(void) {                          /* heap yok: kayıtları tek tek çek */
    wifi_ap_record_t r; int8_t pr = -127, br = -127; uint8_t pc = 0, bc = 0;
    while (esp_wifi_scan_get_ap_record(&r) == ESP_OK) {
        if (!strcmp((char *)r.ssid, cfg.p.ssid) && r.rssi > pr) { pr = r.rssi; pc = r.primary; }
        if (cfg.b.ssid[0] && !strcmp((char *)r.ssid, cfg.b.ssid) && r.rssi > br) { br = r.rssi; bc = r.primary; }
    }
    esp_wifi_clear_ap_list();                             /* idempotent; kalan varsa serbest bırak */
    if (s.st == ST_CONNECTED) { if (pc && pr >= -75) { s.self_disc = true; esp_wifi_disconnect(); s.st = ST_DISCONNECTING; } return; }
    if (pc)            start_connect(&cfg.p, pc, false);
    else if (bc)       start_connect(&cfg.b, bc, true);
    else if (cfg.p.hidden) start_connect(&cfg.p, 0, false);
    else               enter_retry_wait();
}
static void on_sta_disconnected(const wifi_event_sta_disconnected_t *e) {
    s.reason = e->reason; ip_clear();
    if (s.st == ST_DISCONNECTING) { s.self_disc = false; s.pending ? start_connect(&cfg.p, 0, false) : go_ap_only(); return; }
    if (s.st == ST_CONNECTED) { evlog_wifi_lost(e->reason, e->rssi); s.k = 0; }
    else if (s.st != ST_CONNECTING) return;                /* bayat olay */
    if (s.k < 5) { s.k++; s.st = ST_CONNECTING; esp_timer_start_once(t_retry, 1000000ULL * s.k); }
    else if (!s.backup && cfg.b.ssid[0]) start_connect(&cfg.b, 0, true);
    else enter_retry_wait();                               /* hata sınıfı = s.reason, evlog bir kez */
}
```

### Risk

- `esp_timer` geri çağrıları **esp_timer görevinde** koşar (olay görevinde değil); `t_retry` içinden `esp_wifi_connect()` çağırmak güvenlidir (API thread-safe RPC) ama durum değişkenlerine iki görevden dokunulur → ya zamanlayıcı callback'i yalnız `esp_event_post(LS_EVENT, …)` ile olay kuyruğuna iş bırakır (**önerilen**, tek yazar kuralı korunur) ya da küçük bir spinlock.
- `DISCONNECTING`'de olay gelmezse (sürücü hatası) kilitlenmemek için 2 s koruma zamanlayıcısı.
- Olay görevi yığını: handler'da `wifi_config_t` (~200 B) + `wifi_ap_record_t` (~90 B) + log; 4096 ALTE değeri korunur; su izi `/api/device`'ta raporlanır.
- `CONNECTED + DISC` sonrası tarama atlanıp aynı hedefe 5 deneme yapılır; router kanal değiştirdiyse `FAST_SCAN + channel` ipucu ilk denemede yanlış kanaldan başlar ama sonra tüm kanalları gezer (`D:1397-1399`) — yalnız gecikme, hata değil.

---

## 4. TX gücü

### Bulgu

`esp_wifi_set_max_tx_power(int8_t)` birimi 0,25 dBm, aralık [8, 84]; yalnız `esp_wifi_start()` sonrası (`ESP_ERR_WIFI_NOT_STARTED`); WiFi başlamadan önceki tavan PHY init verisidir (`W:1116-1134`). Tam nicemleme tablosu (`W:1121-1124`):

| Verilen değer | Etkin kayıt | Etkin dBm |
|---|---|---|
| 8–19 | 8 | 2 |
| 20–27 | 20 | 5 |
| 28–33 | 28 | 7 |
| 34–43 | 34 | 8,5 |
| 44–51 | 44 | 11 |
| 52–55 | 52 | 13 |
| 56–59 | 56 | 14 |
| 60–65 | 60 | 15 |
| 66–71 | 66 | 16,5 |
| 72–79 | 72 | 18 |
| 80–84 | 80 | 20 |

ALTE'nin tam sayı dBm×4 çağrısı (`A:821`) bu tabloda: 8→7; 9,10→8,5; 11,12→11; 13→13; 14→14; 15,16→15; 17→16,5; 18,19→18; 20→20 dBm. Yani 13 GUI değeri **9 etkin kademe**; RSSI tablosundaki 8/11/14/17 fiilen 7/11/14/16,5 dBm.

- Değer **cihaz geneli**: "wifi_country_t.max_tx_power'a eşlenir" (`W:1120`) → AP ve STA ortak; ALTE'nin varsayımı doğru.
- Yeniden ilişkilendirme gerekmez: yerel PHY sınırıdır, 802.11 sinyalleşmesi yok; bir sonraki TX'ten itibaren geçerli (**belge sessiz, koddan/deneyimden çıkarım**; ALTE 10 s'de bir değiştiriyor ve bağlantı kopmuyor).
- C6 üst sınırı: `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` aralık 10–20, varsayılan 20 (`PK:84-91`); SoC veri sayfası 802.11b için +21 dBm, WROOM-1 modülü tipik 20,5 dBm; IDF API tavanı 20 dBm (80). 384 mA'lık pik akım iddiası modül veri sayfasında (11b 1 Mbps, 20,5 dBm; SparkFun forumu aktarımı — **veri sayfasından doğrulanacak**).
- PHY kalibrasyonu: `ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y` (varsayılan) kalibrasyon verisini NVS'de tutar, açılışta kısmi kalibrasyon (`PK:8-20`); `ESP_WIFI_NVS_ENABLED=n` bunu **etkilemez** (ayrı namespace). Kalibrasyon TX gücü tavanından bağımsız.
- `CONFIG_ESP_PHY_REDUCE_TX_POWER` (varsayılan n, `ESP_BROWNOUT_DET` gerekir): sıfırlama nedeni brownout ise PHY init verisindeki tüm güç parametreleri `PHY_TX_POWER_LOWEST = LIMIT(MAX_TX_POWER×4, 0, 52)` = **52 (13 dBm)** yapılır — o açılış boyunca tavan 13 dBm (`PK:106-111`, `PI:844-852,892-893`, `esp_phy/esp32c6/include/phy_init_data.h:16-21`). "Yumuşak başlangıç" fikrinin IDF'deki tek karşılığı budur; ALTE'nin 8 dBm → 2,5 s → tavan yaklaşımı IDF'de yok, uygulama katmanında kalır.

### Öneri

- Config `wifi.tx_max` değeri **0,25 dBm biriminde kayıt değeri** (izinli küme {28,34,44,52,56,60,66,72,80}); varsayılan **66** (16,5 dBm ≈ ALTE "17"). GUI 9 kademe gösterir: 7 · 8,5 · 11 · 13 · 14 · 15 · 16,5 · 18 · 20 dBm. 2 ve 5 dBm (8, 20) kullanılabilir ama menzil pratikte yok; GUI'ye alınmaz.
- Yumuşak başlangıç: `esp_wifi_start()` döner dönmez `set_max_tx_power(28)` (7 dBm); 2,5 s sonra hedef (ALTE aynen). `esp_wifi_start` ile ilk `set_max_tx_power` arasındaki birkaç ms'lik beacon PHY tavanında (20 dBm) çıkar — kaçınılmaz; brownout bu pencerede olursa bir sonraki açılışta `ESP_PHY_REDUCE_TX_POWER` devreye girer → **açılması önerilir** (sdkconfig), evlog'a "brownout sonrası PHY tavanı 13 dBm" notu.
- RSSI tablosu (etkin değerlerle): ≥−55 → 28 (7); ≥−62 → 44 (11); ≥−70 → 56 (14); ≥−76 → 66 (16,5); aksi tavan. AP istemcisi bağlanınca ≤56 (14). Histerezis aynen (yükselme anında, düşürme 2 örnek).
- `esp_wifi_get_max_tx_power` ile `tx_now` okunabilir (`W:1147`); ayrıca `WIFI_EVENT_STA_BSS_RSSI_LOW` + `esp_wifi_set_rssi_threshold` router linki için olay güdümlü tetik sağlar (`W:1399-1411`, `T:1234-1236`) — AP istemcileri için `esp_wifi_ap_get_sta_list` yoklaması yine gerekli; 10 s periyodik kalsın.

### Risk

- `ESP_PHY_REDUCE_TX_POWER` brownout'un sebebi besleme değilse de (USB kablosu, kısa devre) bir sonraki açılışı 13 dBm'e kilitler; temiz yeniden başlatma düzeltir. GUI'de "tx_phy_cap" gösterilirse kullanıcı anlar.
- Nicemleme belgedeki tablo kadar; PHY sürümüne göre küçük sapmalar olabilir (`esp_wifi_get_max_tx_power` ile doğrulanır).

---

## 5. Güç tüketimi ve bellek

### Bulgu

- Modem uykusu (`WIFI_PS_MIN_MODEM`, varsayılan) **yalnız STA-only modda ve bağlıyken** çalışır (`D:2160,2166-2168`, `W:665-672`). APSTA'da AP sürekli RF açık tutar; `esp_wifi_set_ps(WIFI_PS_NONE)` (`A:527`) APSTA'da etkisizdir, AP kapatılınca (STA-only) anlamlıdır. `MIN_MODEM`: her DTIM'de uyanır; `MAX_MODEM`: `listen_interval` (varsayılan 3 beacon) (`T:374-378,558`, `D:2162-2164`). `ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y` (varsayılan): STA-only ve kopukken RF kapalı (`K:414-419`, `D:2178-2186`).
- Pik akım aşamaları (veri sayfası + IDF yapısından): (1) 802.11b 1 Mbps TX en yüksek pik (yönetim çerçeveleri: probe request, auth/assoc, EAPOL, AP beacon'ları her 100 ms — bunlar düşük hızda gider); (2) tarama = 13 kanalda ardışık probe request salvoları (max güçte); (3) bağlanma = auth/assoc/4-yollu el sıkışma (1 Mbps); (4) GUI sunumu = uzun OFDM TX salvoları (daha düşük pik, uzun süre); (5) açılışta PHY kalibrasyonu (NVS'de veri yoksa tam kalibrasyon). ALTE'nin "pik = TX gücü, ortalama = modem uykusu" ayrımı (`A:14-22,523-527`) belgeyle tutarlı.
- "Yumuşak başlangıç" IDF belgesinde yok; `ESP_PHY_REDUCE_TX_POWER` (§4) ve `ESP_PHY_MAX_WIFI_TX_POWER` tek araçlar.
- Tamponlar (`K:26-150`, `D:4196-4250`): statik RX 10×1,6 KB = 16 KB (`esp_wifi_init`'te ayrılır; C6 HE desteğiyle 2–128); dinamik RX 32 (tepe ≈ 51 KB, pratikte çok altı); dinamik TX 32 (tepe ≈ 51 KB); yönetim tamponları küçük. `ESP_WIFI_IRAM_OPT/RX_IRAM_OPT` IRAM kullanır (boyut kararlarında `01-harita`'ya bakılmış; `project-firmware-size-decisions` hafıza notu).
- ALTE heap ölçümü haritada **yok**: `system_info_t.heap_min_free` ve `/api/system/detail heap_min` alanları var (`01-cekirdek-sayac-cikislar.md:213`, `04-web-sunucu-api.md:146`), RTC `heap_min` (`03-depolama-gunluk-ota.md:236`); üç eşzamanlı TLS'in etkisi "ölçülmeli" diye not düşülmüş (`02-ag-ve-iletisim.md:659,699`).

### Öneri

- Karar: APSTA'da PS ayarı önemsiz; AP kapalıyken `WIFI_PS_NONE` kalsın (ALTE gerekçesi: zayıf sinyalde kararlılık; cihaz şebekeden besleniyor). Config'e ayar eklenmez.
- Tamponlar IDF varsayılanında kalsın (ALTE'de de öyle); heap daralırsa önce dinamik RX/TX 32→16 (tepe −50 KB), statik RX 10→8.
- Heap tahmini (C6 512 KB SRAM; **ölçülecek**, IDF örnek deneyimine dayanır): WiFi+lwIP+olay+zamanlayıcı başlatıldıktan sonra boş heap ≈ 300–330 KB; httpd (yığın 8 KB + soket başına ~1 KB); mDNS görevi 4 KB; TLS oturumu ≈ 45–55 KB (16+4 KB kayıt tamponları + el sıkışma); 3 eşzamanlı TLS ≈ 150 KB. Hedef: en kötü durumda ≥ 80 KB boş; adım 3'te `/api/device` `heap_min` ile doğrulanır, adım 4'te üç kanal sırayla yapılırsa (plan `delivery`) tepe tek TLS'e iner.
- Brownout teşhisi için evlog bağlamı ALTE'deki gibi (mod, istemci, RSSI, TX) kalsın.

### Risk

- APSTA'da ortalama akım ~RX sürekli (C6 ≈ 80–90 mA sınıfı) — besleme tasarımı buna göre; "AP kapalı + MIN_MODEM" tasarruf seçeneği ileride istenirse tek satır.

---

## 6. AP (soft-AP)

### Bulgu

- `wifi_ap_config_t` (`T:525-544`): `ssid/ssid_len`, `channel`, `authmode` (WEP/WAPI/OWE desteklenmez; WPA2_PSK/WPA2_WPA3/WPA3'te `pairwise_cipher` açıkça verilmezse CCMP), `ssid_hidden`, `max_connection` (C6 en çok `ESP_WIFI_MAX_CONN_NUM`=10, `TN:22`; varsayılan 10, `D:1448`), `beacon_interval` 100–60000 TU (varsayılan 100), `csa_count` (varsayılan 3), `dtim_period` 1–10, `pmf_cfg`, `sae_pwe_h2e`, `transition_disable`, `bss_max_idle_cfg` (Kconfig'e bağlı), `gtk_rekey_interval` (0 = kapalı).
- Kanal: belge C6 için "aralık dışıysa hata" der (`D:1436`); ALTE `channel=0` ile çalışıyor (`wifi_manager.h:6`, sahada kanıtlı) ama belgelenmemiş. APSTA'da AP, STA'nın kanalına geçer (`W:982-983`) → CSA ile istemciler taşınır.
- AP istemci düşürme: AP 300 s veri almazsa deauth (`W:1356-1357`), `esp_wifi_set_inactive_time(WIFI_IF_AP, ≥10)`; `AP_STADISCONNECTED` yükünde `reason` var (`T:1216-1221`), `AP_STACONNECTED {mac, aid, is_mesh_child}` (`T:1206-1211`).
- `esp_wifi_ap_get_sta_list` → `wifi_sta_info_t.rssi` "bağlı STA'nın güncel ortalama RSSI'si" (`T:614-616`); ölçülmeden önce 0 (ALTE atlıyor, `A:842`).
- Varsayılan AP netif IP `192.168.4.1/24` (`esp_netif_defaults.h:63,275` `_g_esp_netif_soft_ap_ip`); DHCP sunucusu kira 120 × 60 s = 2 saat (`dhcpserver.h:74`, `LK:400-406`), en çok 8 istemci (`LK:410-416`).
- **DNS seçeneği:** `CONFIG_LWIP_DHCPS_ADD_DNS=y` (varsayılan, `LK:428-434`) → DNS ayarlanmamışsa DHCP yanıtına option 6 = **AP'nin kendi IP'si** eklenir (`dhcpserver.c:466-485`). `esp_netif_set_dns_info(ap, MAIN, …)` DHCPS'teki sunucuyu değiştirir (`NL:2069-2076`), `esp_netif_dhcps_option(SET, ESP_NETIF_DOMAIN_NAME_SERVER, &1/0)` sunmayı açar/kapatır (`N:520-560`, `NL:2555-2560`). Cihazda DNS sunucusu **yoktur**; dolayısıyla AP istemcisinin 53'e gönderdiği sorgular cevapsız kalır. `ls-xxxx.local` çözümü mDNS ile olur; mDNS 1.14.0 varsayılan olarak STA **ve** AP netif'lerinde cevap verir (`M/Kconfig:188-199` `MDNS_PREDEF_NETIF_STA/AP=y`).

### Öneri

- AP cfg: `ssid=cihaz adı`, `ssid_len`, `channel=1` (açık; APSTA'da sürücü STA kanalına taşır), `authmode=OPEN`, `max_connection=4`, `beacon_interval=100`, `pmf_cfg.required=false`, `csa_count=3` (varsayılan). İleride parola: `WIFI_AUTH_WPA2_PSK` + `pairwise_cipher=CCMP` (≥8 karakter); WPA3 istenirse `WPA2_WPA3_PSK` + `sae_pwe_h2e=BOTH` + `pmf_cfg.required=true` (WPA3 AP'de PMF zorunlu, `S:95`). Config şeması bugünden `ap.pass` alanını içerebilir (boş = açık).
- DNS: hiçbir şey yapma (varsayılan option 6 = 192.168.4.1 zararsız; telefon ".local"ı mDNS ile çözer, GUI zaten IP'yi gösterir). Captive portal **yapılmaz** (plan). "hostname DNS'le çalışsın" isteniyorsa ileride tek-sorgu UDP/53 yanıtlayıcı (yalnız kendi adını yanıtlar, gerisine NXDOMAIN) `yapilacaklar.md`'ye.
- `esp_wifi_set_inactive_time(WIFI_IF_AP, 120)` ile hayalet istemciler 2 dk'da düşer (TX değerlendirmesi ve istemci sayısı doğru kalır).

### Risk

- `channel=0` → 1 geçişinde ALTE'den farklı davranış yok; test edilecek.
- 4 istemci sınırı ESP-NOW şifreleme anahtarlarıyla paylaşılır (`D:1448`), ESP-NOW kullanılmıyor → sorun yok.

---

## 7. SNTP, hostname, mDNS

### Bulgu

- `esp_netif_sntp_init(&cfg)` tek sefer; ikinci çağrı `ESP_ERR_INVALID_STATE` ("already initialized", `NS/.c:93`). `ESP_NETIF_SNTP_DEFAULT_CONFIG(server)`: `smooth_sync=false`, `server_from_dhcp=false`, `wait_for_sync=true` (semafor yaratır), `start=true`, `sync_cb=NULL`, `renew_servers_after_new_IP=false` (`NS/.h:36-53`). `smooth_sync=true` → `SNTP_SYNC_MODE_SMOOTH` (`adjtime` ile kademeli; fark >35 dk ise anında) (`NS/.c:65-66`, `esp_sntp.h:25,61`). `esp_netif_sntp_sync_wait(tout)`: semafor bekler; smooth modda hâlâ sürüyorsa `ESP_ERR_NOT_FINISHED` (`NS/.c:155-168`). Yenileme aralığı `CONFIG_LWIP_SNTP_UPDATE_DELAY` 1 saat (`LK:1153-1159`); ilk istek `CONFIG_LWIP_SNTP_STARTUP_DELAY=y` ile 0–`LWIP_SNTP_MAXIMUM_STARTUP_DELAY`(5000 ms) rastgele gecikir (`LK:1162-1178`, `lwipopts.h:1595-1598`). `LWIP_SNTP_MAX_SERVERS` varsayılan 1.
- `esp_netif_set_hostname(netif, name)`: `CONFIG_LWIP_LOCAL_HOSTNAME` ("espressif") yerine geçer; en çok 32 bayt; **arayüz başladıktan/bağlandıktan sonra değiştirilirse yeniden bağlanınca yansır** (`N:350-364`, `NL:1745-1783`; lwIP görevinde koşar). lwIP DHCP istemcisi `netif->hostname` varsa DISCOVER/REQUEST'e **option 12** ekler (`lwip/src/core/ipv4/dhcp.c:505,1125,1563-1575`). ALTE hiç çağırmıyor (harita bulgu 15) → router listesinde "espressif".
- mDNS (`espressif/mdns` 1.14.0): `mdns_init` olay işleyicisi kaydeder, kendi görevi (öncelik 1, yığın 4096, `M/Kconfig:19-39`); `mdns_hostname_set` küçük harf önerisi RFC 6762 (ALTE `A:417-419`); `_http._tcp` 80. IPv6 kapalıyken (`CONFIG_LWIP_IPV6=n`, ALTE `sdkconfig.defaults:72`) AAAA kaydı derlenmez, yalnız A (`M/mdns_debug.c:359`, `mdns_console.c:195` `CONFIG_LWIP_IPV6` korumaları). `MDNS_ENABLE_BROWSE=n` boyut için (ALTE). GOT_IP'de `mdns_hostname_set` tekrarı (`A:542-544`) gereksiz: kütüphane IP olaylarını kendisi izler (`mdns_init` açıklaması, `M/include/mdns.h:179-185`).
- DHCP hostname ile mDNS hostname **bağımsız** iki kayıttır: ilki router'ın istemci listesi/yerel DNS'i (`ls-xxxx.lan`, `fritz.box`), ikincisi `.local`.

### Öneri

- `esp_netif_create_default_wifi_sta()` hemen ardından, `esp_wifi_start()`'tan **önce**: `esp_netif_set_hostname(sta, "LS-XXXXXXXXXX")` (AP netif'ine de aynı ad; `esp_netif_get_hostname` tutarlılığı için). DHCP option 12 = cihaz adı (büyük harf serbest; DNS büyük/küçük duyarsız). mDNS: küçük harf (`ls-xxxx.local`).
- SNTP: ilk `GOT_IP`'de bir kez; `cfg.sync_cb = evlog_time_synced`, `cfg.wait_for_sync = false` (semafor yok; olay güdümlü), `smooth_sync=false` (anında; saat yalnız log/`{time}` için), sunucu `pool.ntp.org` sabit (config'e almak isteğe bağlı). Yeniden bağlanmalarda `esp_netif_sntp_start()` ile yenileme (durdur+başlat, `NS/.c:170-180`) gereksiz; SNTP kendi saatine göre devam eder.
- mDNS: `mdns_init` + `mdns_hostname_set` + `mdns_instance_name_set` + `_http._tcp` açılışta bir kez (ALTE); GOT_IP'de tekrar yok. IPv6 kapalı kalır (boyut kararı).

### Risk

- Hostname 32 bayt sınırı (cihaz adı 13 karakter → sorun yok).
- İlk SNTP yanıtı en erken 0–5 s + ağ gecikmesi; evlog açılış epoch'u ancak o zaman düzelir (ALTE'deki gibi `approx` işaretiyle).

---

## 8. WPA3 / SAE / PMF

### Bulgu

- `ESP_WIFI_ENABLE_WPA3_SAE=y` (varsayılan) `ESP_WIFI_MBEDTLS_CRYPTO` seçer; `ESP_WIFI_ENABLE_SAE_H2E=y`, `ESP_WIFI_ENABLE_SAE_PK=y`, `ESP_WIFI_SOFTAP_SAE_SUPPORT=y` varsayılan (`K:301-342`).
- `threshold.authmode` bir **alt sınırdır**: eşikten daha güvenli olan her ağ kabul edilir (`D:1418-1420`); `WIFI_AUTH_WPA2_PSK` eşiği WPA2, WPA2/WPA3 geçiş ve WPA3-only router'a bağlanmayı **engellemez**; STA mevcut en yüksek modu (WPA3) kendisi seçer (`S:99`). Eşik hiç verilmezse ve parola ≥8 karakterse sürücü yine WPA2'ye çeker (`T:366`). WPA/WEP router'lar 211 ile reddedilir.
- PMF: `capable` **kullanımdan kalktı, her zaman true**; `required=false` varsayılan "PMF Optional" → WPA2 (PMF'siz) ve WPA3 (PMF zorunlu) router'ların ikisi de çalışır (`T:490-493`, `S:36-40`). `required=true` yalnız PMF'li router'lara bağlanır — ev router'larında risk.
- `sae_pwe_h2e`: `WPA3_SAE_PWE_UNSPECIFIED/HUNT_AND_PECK/HASH_TO_ELEMENT/BOTH` (`T:498-503`); yalnız HnP ayarlıysa H2E-only AP'de 210 (`D:1202-1215`); `BOTH` ikisini de dener (`S:88`). `transition_disable` bayrağı ağ WPA3'ü tam desteklediğinde geçiş modunu kapatır (`S:84`); `sae_pk_mode` otomatik.
- AP tarafında WPA3 isteniyorsa `WIFI_AUTH_WPA3_PSK`/`WPA2_WPA3_PSK` + PMF required (örnek `examples/wifi/getting_started/softAP/main/softap_example_main.c:74-84`).

### Öneri

STA cfg: `threshold.authmode = parola ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN`, `pmf_cfg.required=false`, `sae_pwe_h2e=WPA3_SAE_PWE_BOTH`, `transition_disable=0`, `owe_enabled=0`, `sae_pk_mode=AUTOMATIC` (0). Kconfig: WPA3 açık kalır (hafıza notu "keep WPA3"), `ESP_WIFI_ENABLE_SAE_PK=n` ve `ESP_WIFI_SOFTAP_SAE_SUPPORT=n` boyut için kapatılabilir (AP açık/WPA2 planlı; `ESP_WIFI_ENABLE_WPA3_OWE_STA=n` de). GUI'de 211 → "Router WPA/WEP kullanıyor; WPA2 ya da WPA3'e geçin".

### Risk

- SAE el sıkışması mbedTLS ECC kullanır → bağlanma süresi WPA2'ye göre uzar (yüzlerce ms); 5 denemeli zamanlama etkilenmez.
- Yanlış parola WPA3'te 202 mi 15 mi, ölçülecek (§2).

---

## 9. Plana etkisi

| Plan maddesi | Değişiklik |
|---|---|
| §5.4 "reason 2/8 yeniden deneme yok" | **Kaldır.** Kendi kopmamız `self_disc` bayrağı + `DISCONNECTING` durumu ile tanınır; 2/3/8 dâhil tüm dış kopmalar yeniden denenir. |
| §5.4 durum diyagramı | `DISCONNECTING` eklenir; `RETRY_WAIT` uzun bekleme, 1..5 s denemeler `CONNECTING` içinde; `EXHAUSTED` karar anı (ayrı durum değil). Geçiş tablosu §3. |
| §5.4 "15 s'de bir tarama" | Uyarlanır: ilk 2 dk 15 s, sonra 60 s; AP istemcisi varken ≥60 s. Tarama 0/120 ms, `home_chan_dwell_time` istemci varken 100 ms. |
| §5.4 "birincil → yedek → gizli-kör" | Kör deneme yerine config `wifi.p.hidden` / `wifi.b.hidden` bayrağı (GUI onay kutusu). Bağlanırken `sta.channel` ipucu + `WIFI_FAST_SCAN`. |
| Karar 17 (30 dk birincil) | Koparmadan arka plan taraması; birincil görünür ve RSSI ≥ −75 ise geç. |
| §5.4 TX gücü | Config `tx_max` 0,25 dBm kayıt değeri (küme 28…80, varsayılan 66); GUI 9 kademe; RSSI tablosu 28/44/56/66; AP istemcisinde ≤56. `CONFIG_ESP_PHY_REDUCE_TX_POWER=y` önerilir (sdkconfig). |
| §5.4 `STA_LOST_IP` | `CONNECTED`'da gelirse `self_disc` + disconnect → temiz yeniden bağlanma; `GOT_IP` ip=0 aynı yol; 120 s gecikmeli olduğu belgelenir. |
| §5.4 hostname | `esp_netif_set_hostname` iki netif'e, `esp_wifi_start` öncesi; DHCP option 12 doğrulanır (router listesi). |
| §5.4 SNTP | `wait_for_sync=false`, `smooth=false`, `sync_cb`; GOT_IP'de mDNS hostname tekrarı kaldırılır. |
| §5.4 AP | `channel=1` açık; `esp_wifi_set_inactive_time(AP, 120)`; `ap.pass` alanı şemada (boş = açık); DNS'e dokunulmaz, captive yok. |
| §5.4 modem uykusu | Açıklama: APSTA'da PS etkisiz; `WIFI_PS_NONE` yalnız AP kapalıyken anlamlı; ayar değil sabit. |
| API `GET /api/wifi` | `last_error {class, reason, rssi}`, `tx_now` (0,25 dBm), `ap_auto_opened`, `scan_interval` eklenir. |
| §5.8 sdkconfig | `ESP_WIFI_NVS_ENABLED=n` (PHY kalibrasyonu NVS'de kalır), `ESP_PHY_REDUCE_TX_POWER=y`, `ESP_WIFI_ENABLE_SAE_PK=n`, `ESP_WIFI_SOFTAP_SAE_SUPPORT=n`, `ESP_WIFI_ENABLE_WPA3_OWE_STA=n`, `ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096` (ALTE). |
| §6.5 denetim | Tezgâh ölçümleri: (a) telefon AP'de iken 120/30 vs 120/100 taramada kopma sayısı; (b) `AP_STOP/START` AP→APSTA geçişinde; (c) WPA3 yanlış parola reason'ı; (d) `heap_min` üç TLS'te; (e) router istemci listesinde hostname; (f) `channel=1` ile CSA sonrası telefon davranışı. |
| `yapilacaklar.md` | AP istemcileri için minimal DNS yanıtlayıcı (yalnız kendi adı); RSSI'ye göre birincil/yedek seçimi eşiği; AP kapalıyken `MIN_MODEM` seçeneği. |
