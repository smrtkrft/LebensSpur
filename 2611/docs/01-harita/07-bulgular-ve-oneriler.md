# 07 — Bulgu kaydı ve tasarım ilkeleri

Haritalama (01–06) sırasında bulunan sorunların birleşik kaydı ve her birinden türeyen 2611 tasarım ilkesi. Plan (02/04) ve "daha iyi sistem yapılandırması" aşamasının zorunlu girdisidir. Ayrıntı ve satır numaraları kaynak belgelerde.

Önem: **K** kritik (ürünün varlık sebebine ya da veri güvenliğine dokunur) · **Y** yüksek · **O** orta.

## 1. Bulgu kaydı

### 1.1 Çekirdek: sayaç, tetikleme, röle (kaynak: 01)

| # | Önem | Bulgu | 2611 ilkesi |
|---|---|---|---|
| C1 | K | Tetikleme kalıcı değil: süre dolunca NVS'ye yalnız `enabled=0` yazılıyor; mail gönderimi sırasında güç/WiFi giderse protokol sessizce kaybolur. Yeniden deneme yok. | **Tetikleme bir durumdur, olay değil.** `TRIGGERED` durumu ve teslimat kuyruğu kalıcı; açılışta kaldığı yerden devam eder; her kanal teslim edilene kadar yeniden denenir. |
| C2 | K | Tetiklemede Telegram hiç gönderilmiyor (`ls_actions`'ta `trig_tg` yok). | Her olay (erken uyarı, tetikleme) için her kanalın açık/kapalı durumu tek şemada, GUI'de görünür. |
| C3 | K | Erteleme röleyi güvenilir kapatmıyor: düğme yalnız `relay_is_on` doğruysa kapatır (gecikme/darbe fazında yanlış); web ve API ertelemesi röleye hiç dokunmaz; eski röle worker'ı yenisinin durumunu bozar. | **Tek erteleme kapısı:** `checkin(source)` → sayaç sıfırla + röleyi söndür + günlüğe yaz. Düğme, web, API, ileride CLI hepsi buradan geçer. Röle tek durum makinesi, tek görev. |
| C4 | Y | esp_timer görevinde (öncelik 22, 3.5 KB yığın) bloklayan iş: LittleFS yazımı, NVS, 1.5 s `vTaskDelay`. WiFi/lwIP timer'ları dahil her şey bekler. | **Timer callback'leri yalnız olay üretir.** Tek bir "uygulama olay görevi" (kuyruk) işi yapar. Callback'te I/O yasak. |
| C5 | Y | Sayaç yalnız cihaz açıkken ilerler; kapalı süre sayılmaz; duvar saati kullanılmıyor; NVS kontrol noktası 60 s. | Ürün kararı gerekli (bkz. §3). SNTP varken son-başarılı-saat + monotonik süre birleşimi mümkün. |
| C6 | Y | Tatil sürerken iptal ortak esp_timer'ı siler → tatil sayacı donar; tatil iptali alarm sayacını sıfırlamaz. | Sayaç durum makinesi tek tabloda (IDLE / RUNNING / WARNING_n / VACATION / TRIGGERED), geçişler tek fonksiyonda, birim testi ile. |
| C7 | O | Alarm sayısı `değer/2` ile kalıcı kırpılıyor, ölçüt birim adedi: "1 gün" → 0 erken uyarı. | Doğrulama saniye cinsinden: `uyarı_sayısı × uyarı_aralığı < süre`. Kırpma yerine hata döndür. |
| C8 | O | `xTaskCreate` dönüşleri denetlenmiyor; heap biterse mail/röle sessizce atlanır. | Uzun ömürlü görevler açılışta bir kez yaratılır; iş kuyruklarla gelir. Yaratma başarısızlığı ölümcül hata olarak loglanır. |
| C9 | O | `inverted` rölede pin `output_init`'e kadar sürülmediği için her açılışta "tetiklenmiş" darbesi. | GPIO güvenli seviyesi `app_main`'in ilk satırında, dosya sistemi/NVS'den önce. |
| C10 | O | `ESP_ERROR_CHECK(nvs_flash_init)` NVS bozuksa açılış döngüsü. | NVS bozuksa: logla, silme kararını kullanıcıya bırakma seçeneği yoksa sil ama **açılış kaydına düşür**. |

### 1.2 Web sunucusu ve API (kaynak: 04)

| # | Önem | Bulgu | 2611 ilkesi |
|---|---|---|---|
| W1 | K | `Set-Cookie` use-after-return: yerel tampon `httpd_resp_set_hdr`'a veriliyor; IDF işaretçiyi saklar. | Başlık değerleri istek ömrü boyunca yaşayan bellekte (istek bağlamı yapısı). Denetim listesine madde. |
| W2 | K | CSRF/Origin denetimi yok; port 80 düz HTTP; AP parolasız → parola yoksa herhangi bir web sayfası fabrika sıfırlama/röle tetikleyebilir. | Durum değiştiren her istek: `Origin`/`Host` eşleşmesi **veya** özel başlık (`X-LS-Request: 1`) zorunlu. AP parolası cihaz-özel varsayılan (etikette). HTTPS sunucu boyut nedeniyle yok; bu açıkça belgelenir. |
| W3 | Y | SMTP/Telegram/webhook testleri httpd görevinde senkron; TLS el sıkışması 8 KB httpd yığınında. | **Uzun iş = görev + durum.** Tek örüntü: `POST …/test` → iş kuyruğuna → `GET …/status`. httpd hiçbir zaman ağ istemcisi çalıştırmaz. |
| W4 | Y | Kesilmiş gövde → sessiz veri kaybı: `read_body` tampon-1 okuyor, parse başarısız olunca alanlar boş yazılıp `ok` dönüyor; `delete`'te `id` eksikse grup 0 siliniyor. | Gövde sınırı aşımı → **413**; JSON hatası → **400**; zorunlu alan yoksa → 400. Kısmi yazma yok: önce doğrula, sonra tek seferde kaydet. |
| W5 | Y | Heap taşması `trigger-mail/files` (`int pos` → `size_t`). | Yanıtlar cJSON ile üretilir ya da akışla (chunked) yazılır; elle `snprintf` ofset aritmetiği yok. |
| W6 | O | Hata biçimi 6 farklı şekilde; `read_body → 400` bloğu 17 kez kopyalanmış; her alan için gövde baştan parse ediliyor (9 kez). | Tek yanıt zarfı: `{"ok":true,"data":…}` / `{"ok":false,"error":{"code","message"}}`. Rota tablosu + ortak ön işleyici (auth, gövde, parse) bir kez. |
| W7 | O | Küresel giriş kilidi (IP bağımsız → DoS); `/api/reset` anahtarı 32 bit, maske 4 hex sızdırıyor, hız sınırı yok. | Anahtar ≥128 bit (base32, 26 karakter); sabit zamanlı karşılaştırma; dakikada N deneme; anahtar üretimi/değişimi günlüğe. |
| W8 | O | `s_sse_fds` kilitsiz; `web_server_stop` hiç çağrılmıyor; WiFi tarama API'ye açılmamış; `relay/test` `duration=0` ise röle kalıcı açık. | SSE istemci listesi mutex altında; röle testi her zaman süreli; tarama ucu eklenir (GUI'de var olan özellik). |
| W9 | O | Captive portal yok; AP/STA'da sunucu farkı yok. | AP modunda DNS yakalama (captive) **yeni özellik sayılır, 2611'e girmez**; nota alınır. |

### 1.3 Depolama, günlük, OTA (kaynak: 03)

| # | Önem | Bulgu | 2611 ilkesi |
|---|---|---|---|
| S1 | K | OTA imza doğrulaması yok; özel URL'de `http://` kabul; AP parolasız → parola ayarlanmamış cihaza RF menzilindeki herkes firmware yükleyebilir. | OTA yalnız `https://`; sürüm düşürme reddi (semver karşılaştırma); OTA ucu her zaman yetkili (parola yoksa bile cihaz-özel AP parolası var). Secure Boot/Flash Encryption **üretim öncesi** adımı olarak planda; eFuse yazımı bench'te yapılmaz. |
| S2 | K | Sırlar (WiFi, SMTP, Telegram, MQTT) NVS'de düz metin; NVS şifreleme kapalı. | `CONFIG_NVS_ENCRYPTION` + HMAC tabanlı anahtar planda (üretim adımı). Tasarım bunu engellemeyecek şekilde: sırlar tek namespace'te. |
| S3 | Y | GUI OTA gzip commit'inden beri fiilen bozuk (Slot B → A'ya düşme). | Kalkıyor (karar verildi). |
| S4 | Y | Sessiz veri kaybı: NVS sürüm değişiminde tüm ayarlar silinir (günlüğe düşmez); bozuk LittleFS sessizce biçimlenir; fabrika sıfırlama günlüğü de siler. | Her silme/biçimlendirme açılış kaydına düşer; fabrika sıfırlama günlüğü **korur** (yalnız yapılandırma ve kullanıcı dosyaları); yapılandırma yedeği harici flash'ta (JSON dışa/içe aktarma). |
| S5 | Y | Sürüm 5 yerde elle; css/js 1 yıl `immutable` önbellekli; `?v=` unutulursa tarayıcı eski `app.js` kullanır. | **Tek sürüm kaynağı:** `PROJECT_VER` → derlemede `config.json`/`?v=` üretilir. Statik dosya adları içerik özetiyle (`app.<hash>.js`) ya da `?v=` otomatik. |
| S6 | O | Sürüm karşılaştırması yalnız eşitlik; düşürme "güncelleme" sayılıyor. | semver karşılaştırma yardımcısı; GUI "daha yeni var" yalnız gerçekten yeniyse. |
| S7 | O | OTA/user restart nedenleri günlükte yanlış (`SL_RST_OTA_FW` hiç set edilmiyor); OTA sonrası 60 s onay penceresinde herhangi bir reset geri döndürür; DONE sonrası "rollback" bilgisi yanlış. | Yeniden başlatma nedeni her `esp_restart` öncesi RTC'ye yazılır (tek yardımcı). OTA onayı: WiFi bağlandı + HTTP sunucu ayağa kalktı → onay; süre yerine koşul. |
| S8 | O | `/api/logs` 24 KB okuma sınırı; mail ekleri sınırsız yüklenip yalnız ilk 5'i gönderiliyor; `delete_dir_contents` ve naif JSON ayrıştırıcı 4 yerde tekrar. | Günlük okuma sayfalı (son N kayıt, ters okuma); ek sayısı ve toplam boyut yüklemede sınırlanır ve GUI'de gösterilir; dosya yardımcıları tek modülde (`fs_util`). |
| S9 | O | Harici flash yokken cihaz zaten çalışıyor (gömülü GUI, RTC günlük) — yeni hedef bugün "yedek yol". | Harici flash **isteğe bağlı** bileşen: yoksa günlük RAM/RTC'de, ekler kapalı, GUI'de "depolama yok" rozeti. Tek LittleFS bölümü `/data`. |
| S10 | O | 16 MB çipte oluşmuş `user_data`, 32 MB çipe taşınınca biçimlenir (yarı-boyut kuralı). | Bölüm boyutu çipten türetilmez: sabit `/data` başlangıcı, boyut = çip − başlangıç; süper blok boyutu küçükse **biçimleme değil uyarı**. |

### 1.4 Web arayüzü (kaynak: 05)

| # | Önem | Bulgu | 2611 ilkesi |
|---|---|---|---|
| G1 | K | Röle ayarı her "Kaydet"te sessizce sıfırlanıyor (`loadRelayConfig` çağrılmıyor, footer Kaydet HTML varsayılanlarını gönderiyor). | **Tek kaydetme modeli:** her sayfa kendi formunu, yalnız değişen alanları, tek uca gönderir. Form sunucudan dolmadan kaydedilemez. |
| G2 | K | Footer "Kaydet" bağlamdan bağımsız 7–10 POST atıyor (boş WiFi parolasıyla `wifi/connect` dahil). | Yukarıdaki ile aynı; "Kaydet" yalnız görünen formu kaydeder; WiFi değişikliği ayrı, açık onaylı eylem. |
| G3 | Y | SSE açıkken 10 s/15 s polling; login ekranında da; 401'de SSE 3 s'de bir sonsuz yeniden bağlanma; dil dosyası her yüklemede `?v=Date.now()`. | SSE varken polling yok; sayaç istemcide yerel tik ile ilerler, sunucu yalnız olayda ve dakikada bir "hizalama" gönderir; login'de hiçbir arka plan isteği yok; dil dosyaları sürümlü önbellek. **Güç kısıtı gereği.** |
| G4 | O | `t()` yer tutucuyu yalnız ilk geçtiği yerde değiştiriyor; HTML/lang 6 yerde sapmış; HTML'de sabit Türkçe metin. | `replaceAll`; HTML'de metin yok, yalnız `data-i18n`; dil dosyaları derlemede doğrulanır (anahtar kümesi, yer tutucu). |
| G5 | O | 4 kaydetme modeli yan yana; "İptal" geri almıyor; erişilebilirlik sıfır; 32 yalnız-ikon düğme. | Tek form bileşeni; `aria-label`, `<label for>`, klavye; "İptal" = sunucudan yeniden yükle. |
| G6 | O | CSS: 0 değişken, 101 farklı renk, açık tema %34 elle kopya, 43 `!important`, ~28 ölü blok. 68 inline SVG / 36 benzersiz. | CSS değişkenleri (tema = değişken seti), SVG sprite (`<use>`), ölü kural denetimi derlemede. |
| G7 | O | Gömülü 112,8 KB gz: dil %41, JS %20, PNG %19 (400×400), HTML %12, CSS %8. | Dil dosyaları değer-dizisi biçimi (−13 KB), tek PNG + CSS `invert` ya da SVG logo (−10…20 KB), minify (−8 KB). Hedef ≤ 80 KB gz. |
| G8 | O | 12 `innerHTML` noktasında `esc()` kullanılmış; `g.id` ve bazı parametreler kaçışsız. | `textContent` varsayılan; `innerHTML` yalnız sabit şablonlarda; sunucu verisi daima kaçışlı. |

### 1.5 Ağ ve iletişim (kaynak: 02)

| # | Önem | Bulgu | 2611 ilkesi |
|---|---|---|---|
| N1 | K | **SMTP'de sunucu reddi "başarı" sayılıyor**: DATA sonrası 250 gelmezse yalnız WARN, `ESP_OK` döner; RCPT TO 550 yakalanmıyor. main.c "gönderildi" yazar, GUI testi "gönderildi" der. | SMTP yanıt kodları her adımda zorunlu denetim; 4xx → yeniden dene, 5xx → kalıcı hata + günlük + GUI'de görünür. "Gönderildi" yalnız 250 alınınca. |
| N2 | K | Teslimat garantisi yok: süre dolduğunda WiFi yoksa mail DNS/TCP hatasıyla biter, bir daha denenmez. TLS el sıkışma/yazma döngüleri `WANT_READ` ile sonsuz dönebilir; `connect()` için uygulama zaman aşımı yok → mail görevi süresiz asılı kalır, kalan alıcılara geçemez. | Outbox (ilke 2) + her ağ işleminde **toplam** zaman aşımı (bağlantı, el sıkışma, gönderim ayrı ayrı sınırlı); asılı kalan iş iptal edilip bir sonraki alıcıya geçilir. |
| N3 | Y | `mqtts://` çalışmıyor: `crt_bundle_attach` verilmiyor, esp-tls doğrulama seçeneği olmayan kurulumu reddediyor. GUI kabul ediyor. | Tüm TLS istemcileri tek yardımcı üzerinden (`net_tls_defaults()`), sertifika paketi her zaman bağlı. Özellik GUI'de varsa cihazda sınanmış olmalı (denetim listesi). |
| N4 | Y | Sertifika doğrulama üç düzeyde, iki TLS yığını: SMTP elle mbedTLS + `VERIFY_OPTIONAL` ve sonuç hiç okunmuyor (fiilen doğrulama yok, MITM ile parola ele geçer); base64 parola satırı konsola `ESP_LOGI` ile basılıyor. Bağlantı+AUTH kodu `send_with_attachments` içinde kopya. | SMTP de esp-tls üzerinden; doğrulama **varsayılan açık**, kullanıcı isterse "doğrulamayı atla" (uyarılı, günlüklü). Sırlar hiçbir log seviyesinde basılmaz. Tek SMTP oturum kodu, ek var/yok tek yol. |
| N5 | Y | WiFi: AP kapalıyken STA kaybı cihazı erişilemez bırakır, geri açılmaz; bağlantı hata nedeni GUI'ye ulaşmaz (`connect_sta` daima `ESP_OK`); reconnect görevi dışarıdan `vTaskDelete` (tarama ortasında sızıntı); `s_ip_addr` kopmada sıfırlanmıyor; yedekten birincile dönüş yok; `esp_netif_set_hostname` yok. | WiFi tek durum makinesi (IDLE/CONNECTING/CONNECTED/AP_FALLBACK/AP_ONLY) tek görevde, olaylarla; STA N sn kayıpsa AP **her zaman** açılır (erişilebilirlik güvenlik özelliği); son hata nedeni durumda tutulur ve API'de görünür; birincile periyodik dönüş denemesi; hostname = cihaz adı. |
| N6 | O | TX gücü tasarımı sağlam (yumuşak başlangıç 8 dBm → tavan 17; AP istemcisi gelince ≤14; RSSI tablosu; histerezis) ama `esp_wifi_set_max_tx_power` 11 kademeli nicemleme uyguluyor ("8" → 7, "17" → 16,5). | Tasarım aynen taşınır; GUI'de nicemlenmiş gerçek değer gösterilir; kademe tablosu tek yerde. |
| N7 | O | Erken uyarıda 3 görev + 3 eşzamanlı TLS oturumu aynı anda (güç piki, heap); GUI test düğmeleri httpd görevinde. | Outbox **seri** gönderir (bir kerede tek TLS oturumu); güç kısıtı gereği. Testler de aynı kuyruktan. |
| N8 | O | Sabit Türkçe varsayılan metinler (`main.c:189`, `web_server.c:1905`) 6 dilli GUI ile uyumsuz; `ls_actions` varsayılanlarında mail/Telegram/API kapalı, yalnız röle açık. | Mesaj şablonları kullanıcı dilinde, GUI'den düzenlenebilir, varsayılanlar dil dosyasından; kanal varsayılanları ürün kararı (§3). |
| N9 | O | WiFi sürücüsü `CONFIG_ESP_WIFI_NVS_ENABLED=y` ile STA parolasının ikinci kopyasını tutuyor; `clear_primary` bunu silmiyor. | `ESP_WIFI_NVS_ENABLED=n`; kimlik bilgisi tek yerde (yapılandırma şeması). |

### 1.6 Ek bulgu: mail eki akışı (kullanıcı sorusu, 2026-11-05)

| # | Önem | Bulgu | 2611 ilkesi |
|---|---|---|---|
| A1 | Y | Ekler harici flash'tan 570 B parçalarla akıtılıyor (RAM'e alınmıyor — doğru), ama 550 KB için ~965 ayrı TLS kaydı; kesintide yeniden deneme yok; harici flash yoksa ekler sessizce atlanıyor. | Parça 4–8 KB; teslimat kuyruğu kesintide kaldığı mailden yeniden dener; ek atlanırsa kullanıcıya (GUI + günlük) bildirilir. |

## 2. Türetilen mimari ilkeler (özet)

1. **Olay güdümlü tek uygulama görevi.** Timer/GPIO/ağ callback'leri yalnız kuyruğa olay bırakır. Tüm iş mantığı tek görevde, tek durum makinesinde; yarış yok, mutex azalır.
2. **Teslimat katmanı (outbox).** Kanal bağımsız mesaj kuyruğu, kalıcı (NVS/`/data`), yeniden deneme, sonuç günlüğü. SMTP, Telegram, webhook, MQTT aynı `channel_send()` arayüzü. Tetikleme kalıcı durum.
3. **Tek erteleme kapısı.** `checkin(source)`: sayaç + röle + günlük. Her kaynak yetkilendirilmiş ve loglanmış.
4. **Tek yapılandırma şeması.** Tüm ayarlar bir C yapısı + şema tablosu (ad, tip, sınır, varsayılan, gizli mi). NVS okuma/yazma, JSON dışa/içe aktarma, GUI formu ve doğrulama **aynı tablodan** üretilir. Sürümlü; eski NVS anahtarlarından taşıma (ürün kararı, §3).
5. **API tek zarf, tek ön işleyici.** Rota tablosu; auth, CSRF, gövde sınırı, JSON parse bir yerde. Uzun iş = görev + durum ucu.
6. **Güvenlik varsayılan açık.** Cihaz-özel AP parolası; `/api/reset` anahtarı güçlü ve hız sınırlı; OTA yalnız https + sürüm düşürme reddi; üretim adımı olarak Secure Boot v2 + Flash Encryption + NVS şifreleme planda (bench'te eFuse yakılmaz).
7. **Depolama ayrımı.** Dahili: firmware ×2, NVS (büyütülmüş), otadata, phy. Harici: tek `/data` (günlük, yapılandırma yedeği, ekler, kullanıcı dosyaları); isteğe bağlı, yokluğu görünür.
8. **Tek sürüm kaynağı.** `PROJECT_VER` → derlemede GUI'ye işlenir; statik dosyalar içerik özetli.
9. **Güç kısıtı tasarım girdisi.** Az byte, az eşzamanlı TX, polling yok, büyük parçalı akış.
10. **Boyut.** Hedef ≤ 1,5 MiB imaj, GUI ≤ 80 KB gz; bilerek korunanlar (WPA3, hata metinleri) değişmez.
11. **Günlük tam ve dürüst.** Her silme/biçimleme/yeniden başlatma nedeni kayıtlı; fabrika sıfırlama günlüğü korur.

## 3. Kullanıcıya sorulacak ürün kararları

Plan aşamasında (02) tek seferde sorulacak:

1. **Kapalı süre sayılsın mı?** Cihaz kapalıyken geçen süre sayaçtan düşsün mü (SNTP ile duvar saati)? Düşerse uzun kesintide açılışta hemen tetiklenebilir; düşmezse kesinti sayaç kazandırır.
2. **Erteleme röleyi kapatsın mı?** Her erteleme (düğme/web/API) röleyi söndürsün mü, yoksa röle yalnız süresi dolunca mı kapanır?
3. **Telegram tetiklemede gönderilsin mi?** Bugün yalnız erken uyarıda. Niyet mi, hata mı?
4. **Alarm sayısı kuralı:** kırpma yerine hata (ve açıklama) kabul mü?
5. **NVS uyumluluğu:** 2611 mevcut cihazlara OTA ile mi gidecek (eski ayarlar korunmalı → taşıma kodu), yoksa temiz kurulum mu?
6. **AP parolası:** cihaz-özel varsayılan parola (etikete yazılır) kabul mü? İlk kurulum deneyimini değiştirir.
7. **Fabrika sıfırlama kapsamı:** günlük korunsun mu?
8. **Tetikleme sonrası:** teslimat tamamlanınca sayaç ne olur — durur mu, kullanıcı ertelerse yeniden başlar mı?
9. **Kanal varsayılanları:** bugün `ls_actions` varsayılanında yalnız röle açık, mail/Telegram/API kapalı. Yeni kurulumda hangi kanallar açık gelsin?
10. **SMTP sertifika doğrulaması:** varsayılan **açık** (güvenli) + kullanıcı isterse kapatabilir, kabul mü? Bugün fiilen kapalı ("teslimat önceliği").

## 4. Kaynak belgeler

- `01-cekirdek-sayac-cikislar.md` — §8 sorunlar (21), §9 korunacak (~45), §10 açık sorular (12)
- `03-depolama-gunluk-ota.md` — NVS envanteri (13 namespace, ~60 anahtar), §8 sorunlar, §10 yeni mimari notları
- `04-web-sunucu-api.md` — 55 rota tablosu, §8 sorunlar, §9 korunacak sözleşme
- `05-web-arayuz.md` — 58 satır API tablosu, §9 sorunlar (22), §10 korunacak (38)
- `06-cihaz-ve-urun.md` — donanım, besleme geçmişi, bench
- `02-ag-ve-iletisim.md` — §0 görev/zaman aşımı tabloları, §1–4 modüller, §7 modüller arası sorunlar, §8 korunacak, §9 açık sorular
