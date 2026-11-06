# LebensSpur — yeniden yapılandırma süreci (kod adı 2611)

Başlangıç: 2026-11-05. Referans: `ALTE/HW` + `ALTE/GUI` (aktif hat, v0.0.70).

## Hedef

ALTE'nin yaptığı her işi yapan, ama çok daha iyi toparlanmış yeni bir yapı. Yeni özellik yok.

Temel kararlar (kullanıcı, 2026-11-05):

- Program ve altyapının tamamı (firmware + web arayüzü) **yalnızca dahili 4 MB flash'ta**.
- Harici 16 MB flash **yalnızca** kullanıcı verisi, sonradan yapılan yapılandırmalar ve özel dosyalar için.
- GUI'nin harici flash slotlarına kopyalanması ve ayrı GUI OTA'sı kalkar; tek güncelleme kanalı firmware OTA.
- Kod ALTE'den kopyalanabilir ama yapı sıfırdan kurulur.

## Adımlar

| # | Adım | Durum | Çıktı |
|---|------|-------|-------|
| 1 | Kod ve cihaz işlevini anla, haritala, not çıkar | tamam (2026-11-05) | `docs/01-harita/01…07.md` — 6 harita + bulgu kaydı (46 bulgu, 11 ilke, 10 ürün sorusu) |
| 2 | İlk plan | tamam (2026-11-05), kullanıcı onayladı | `docs/02-plan-ilk.md` — A–D bölümleri onaylı, 22 ürün kararı, 15 araştırma sorusu |
| 3 | Plandaki özelliklerin daha iyisi var mı araştır | tamam (2026-11-05) | `docs/03-arastirma.md` özet + `docs/03-arastirma/01…07` (2 726 satır): 14 plan düzeltmesi, ~60 iyileştirme, 5 kullanıcı kararı (K1–K5) |
| 4 | Son plan | tamam (2026-11-05) | `docs/04-plan-son.md` |
| 5 | Uygulama (adım adım, her adım derleme + flash + commit) | tamam (2026-11-06) — alt adım 0–6 (6: OTA ve cihaz uçları), ilk yayın `v1.0.0` | `code/` (`main`, `components`, `gui`, `test`), `docs/07-adim6-sozlesme.md` |
| 6 | Denetim, en ince ayrıntıya kadar | sıradaki | `docs/05-denetim.md` |

## Karar günlüğü

| Tarih | Karar | Gerekçe |
|-------|-------|---------|
| 2026-11-05 | Yalnızca dahili flash; harici flash sadece veri | GUI dört kopya tutuluyordu (ota_0, ota_1, Slot A, Slot B); tek sürüm, tek OTA kanalı; harici flash'ın tamamı depolamaya kalır |
| 2026-11-05 | Kapalı süre sayaçtan **düşmez** (bugünkü davranış korunur) | Kullanıcı kararı; duvar saati bağımlılığı istenmedi |
| 2026-11-05 | Tetikleme + teslimat sonrası sayaç **durur**; erteleme yeni tur başlatır | TETİKLENDİ kalıcı durum, GUI'de belirgin |
| 2026-11-05 | **Her erteleme** (düğme/web/API) röleyi söndürür | Tek erteleme kapısı; kaynak fark etmez |
| 2026-11-05 | ~~Alarm mantığı bugünkü haliyle taşınır~~ → **yeni alarm modeli C**: 1…10 uyarı, sona doğru logaritmik sıklaşan pencere, otomatik ilk uyarı `min(T/2, 3 gün)`, tepki payı, sessiz kırpma yok | `docs/06-alarm-tasarim.md`; A/B/C/D/E karşılaştırması |
| 2026-11-05 | Telegram **yalnızca erken uyarıda** (bugünkü davranış) | Kullanıcı kararı: Telegram sahibine uyarı kanalı |
| 2026-11-05 | SMTP sertifika doğrulaması **varsayılan açık**, kullanıcı kapatabilir (uyarılı, günlüklü) | MITM ile parola ele geçirme riski kapanır; özel sunucular için kaçış yolu kalır |
| 2026-11-05 | Yapılandırılıp testi geçen kanal **otomatik açık** gelir; röle açık gelir | "Ayarı girdim ama gitmedi" tuzağı kalkar |
| 2026-11-05 | Ek okunamazsa metin yine gider, eksik ek alıcıya + günlüğe + GUI'ye bildirilir | Teslimat önceliği, sessiz kayıp yok |
| 2026-11-05 | **Temiz kurulum**: ALTE NVS'den taşıma yok; ilk açılışta eski NVS silinir, günlüğe düşer | Saha cihazı az; taşıma kodu/test yükü yok |
| 2026-11-05 | Kurulum AP'si **parolasız** kalır, GUI'den parola konabilir | Kullanıcı kararı: kolay kurulum. Tasarım gereği: parola yokken GUI'de belirgin uyarı; OTA özel URL yalnız https |
| 2026-11-05 | Fabrika sıfırlama **iki kademeli / seçimli**: (1) cihaz ayarlarını ilk kurulum haline getir; (2) buna ek olarak harici flash'ı da (günlük, ekler, kullanıcı dosyaları) temizle | Kullanıcı isterse yalnız cihazı, isterse verileriyle birlikte sıfırlar |
| 2026-11-05 | `/api/reset?key=` anahtarı 128 bit base32 (26 kr) + hız sınırı (5 hatalı/dk → 1 dk kilit); uç ve parametre adı aynı | Kaba kuvvet kapanır, dış otomasyon sözleşmesi korunur |
| 2026-11-05 | **Mimari:** iki katman (sistem altyapısı / uygulama), üç görev: `app_core` (asla beklemez, tek olay kuyruğu), `delivery` (seri mesaj gönderimi), `sys_worker` (OTA, depolama işleri). Çalışma anında görev yaratılmaz. OTA sırasında tetikleme → OTA iptal | Kullanıcının "iki çekirdek" fikri + bloklayan/bloklamayan ayrımı |
| 2026-11-05 | **GUI:** vanilla JS, modüler kaynak, esbuild ile bundle + minify, i18n derlemede doğrulanır, sürüm tek kaynaktan | ≤80 KB gz hedefi, çerçeve yok |
| 2026-11-05 | **Yapılandırma:** şema tablosu; asıl kopya dahili NVS (64 KB), her kayıtta JSON yedeği `/data/config/`; NVS boşsa yedekten geri yükleme teklifi | Harici flash olmadan cihaz çalışır; sırlar ileride şifrelenebilir; "sonradan yapılandırmalar harici" ilkesi yedekle karşılanır |
| 2026-11-05 | TETİKLENDİ'de erteleme, başlamamış tetikleme mesajlarını iptal eder | Kullanıcı hayatta; gönderimi süren tamamlanır |
| 2026-11-05 | Fiziksel 5× basma = fabrika **kademe 1**; kademe 2 yalnız GUI'den onaylı | Veri kaybı fiziksel erişimle tetiklenmez |
| 2026-11-05 | Mesaj sınırları konu 128 / gövde 2048 / alıcı 512 | 256 baytlık "kritik mesaj" çok dar |
| 2026-11-05 | JSON yedeği **sırları içerir** | Tam geri yükleme; NVS ile aynı güvenlik sınıfı; NVS şifrelemesi açılınca gözden geçirilir |
| 2026-11-05 | AP kapalıyken STA 10 dk bağlanamazsa AP **otomatik açılır** | Erişilebilirlik; STA dönünce kullanıcı ayarına döner |
| 2026-11-05 | Yedek ağdayken **30 dk'da bir birincil** denenir | Birincile dönüş |
| 2026-11-05 | `build.sh` varsayılan **2611**, `--alte` eski ağaç | Aktif hat 2611 |
| 2026-11-05 | Ek yükleme `PUT /api/groups/<i>/files/<ad>` ham gövde | Cihazda multipart yok |
| 2026-11-05 | 128 KB `coredump` bölümü | Teşhis; kullanıcı itiraz etmedi |
| 2026-11-05 | **K1** OTA için **ayrı GitHub deposu**; `project_name` kapısı zorunlu | `smrtkrft/LebensSpur` `releases/latest` başka ürünü (DMF) gösteriyor; API'siz akış yalnız ayrı depoda çalışır |
| 2026-11-05 | **K2** Harici flash: `/log` 1 MB + `/data` kalan | Günlük otomatik onarım, kademe-2'de hayatta kalır, ek yazımı günlüğü bozamaz |
| 2026-11-05 | **K3** MQTT: `net_tls` üzerinde kendi hafif 3.1.1 tek-yayın istemcisi; esp-mqtt yedek | −14 KB flash, "görev yaratılmaz" kuralı korunur |
| 2026-11-05 | **K4** Yeniden deneme: TRIGGER 0/1/5/15/30 dk → 30 dk'da bir ±%10 jitter, 72 saat açık süre ya da erteleme; EARLY_WARN 3 deneme 0/1/5 dk | RFC 5321 §4.5.4, MTA uygulamaları, greylisting |
| 2026-11-05 | Araştırmadan gelen 14 plan düzeltmesi ve iyileştirmeler kabul (bkz. `03-arastirma.md` §2–3) | Kanıtlı bulgular; ürün davranışını değiştirenler ayrıca soruldu |
| 2026-11-05 | **K5** Brownout sonrası kısa aralıklı açılışta TX tavanı geçici 8 dBm + teslimat 30 s ertelenir; **içerik ve ekler değişmez** | Brownout-reboot döngüsü kırılır, teslimat önceliği korunur |
| 2026-11-05 | OTA release deposu **`smrtkrft/LebensSpur-firmware`** (yalnız release; kaynak bu depoda); varlıklar `lebensspur.bin` + `lebensspur.elf` | Ürün adıyla kalıcı; API'siz akış |

## Çalışma kuralları

- Her adım: yap → denetle → düzelt → yeniden denetle → commit. Ara commit atlanmaz.
- Commit mesajları Türkçe. Yazar yalnızca kullanıcının git kimliği.
- Derleme çıktıları `code/esp_bin/` altında; kaynak ağacına üretilen dosya yazılmaz.
- Her adımdan sonra USB'deki cihaz flash'lanır ve açılış logu alınır.
| 2026-11-05 | Arayüz görünümü **ALTE tasarımına birebir** dayanır (düzen, renkler, halka, sekmeler, akordeonlar, ikonlar); iç yapı ve hatalar düzelir. 3.4'teki sayfa yalnız geçici iskelet. Sıra: önce teslimat (adım 4), sonra arayüz (adım 5) | Kullanıcı kararı; ALTE tasarımı kullanıcının |
| 2026-11-05 | SMTP ve Telegram gerçek hesap testleri denetim adımına (7) bırakıldı | Kullanıcı kararı; webhook/MQTT/MQTTS gerçek ağda doğrulandı |
| 2026-11-05 | Arayüz Kaydet'i yalnız açık sekmede **değişen alanları** gönderir; yüklenmemiş bölüm hiç kaydedilmez | ALTE'de her Kaydet 7–10 istek atıyordu; cihazda yakalanan hata: yüklenmemiş form HTML varsayılanını (24 gün) yazdı |
| 2026-11-06 | **Test kuralı:** her düzeltmeden sonra tam denetim baştan (`code/esp_bin/full_check.sh`: birim, -Werror derleme+flash, API, ekran/JS, arayüz↔cihaz gidiş-dönüş, güvenlik); **iki tam tur arka arkaya hatasız** olmadan sonraki aşamaya geçilmez | Kullanıcı kararı: bir düzeltme başka yeri bozmasın |
| 2026-11-06 | Güvenlik testi (parolasız ↔ parolalı, 163 kontrol) bulguları düzeltildi: güvenlik başlıkları (XFO/CSP/nosniff/Referrer-Policy), SSE akışı oturuma bağlı (parola değişimi/çıkış/süre dolunca kapanır), oturum süresi son kullanımdan + güncel ttl ile, yarım istek beklemesi 2 sn, WiFi/AP ayarı yanıt gittikten 500 ms sonra uygulanır | Parolasızken açıklık tasarım gereği (12 uç), GUI kalıcı uyarı gösterir |
| 2026-11-06 | Sürüm şeması **4 parçalı** (1.0.1.1'den başlar); 1.0.1.1–1.0.1.99 deneme/test yayınları için serbest | Kullanıcı kararı |
| 2026-11-06 | Zamanlayıcı API komutları **eşzamanlı** (`core_post_sync`, ≤1 s): yanıt döndüğünde durum yeni haliyle okunur | Tam denetimde yakalanan yarış (durdur → hâlâ çalışıyor) |
| 2026-11-06 | **Depo düzeni:** `2611/HW` + `2611/GUI` ayrımı kalktı, `2611` → **`code/`** (standart ESP-IDF projesi: `CMakeLists.txt` + `main` + `components` + `gui` + `test` + `tools`; `idf.py` doğrudan çalışır), derleme/test betikleri ve çıktılar `code/esp_bin/` (`sdkconfig` ve `build/` orada; sarmalayıcı proje kalktı), belgeler kökte `docs/`, kullanım `README.md`. ALTE, Neue ve eski notlar `nogit/` (git'e girmez). Eski belgelerdeki `2611/`, `HW/`, `GUI/`, `esp_bin/` yolları tarihsel; depolama işareti `.ls2611` veri biçimi olduğu için değişmedi | Arayüz zaten firmware'in parçası (tek imaj, tek sürüm, tek OTA); GitHub'da düzenli yapı |
| 2026-11-06 | **Röle boşta konumu:** süre bitince ve her yeniden başlatmada röle ön tanımlı (boşta) konumuna döner, tetiklenmiş sınırsız röle açılışta yeniden çekilmez. Açılışta polarite okunmadan pin sürülmez (önceden LOW sürülüyordu: `inverted` bağlantıda röle ~onlarca ms çekiyordu); ayarlar okununca önce seviye, sonra çıkış. Her `esp_restart` öncesi kapanış işleyicisi boşta seviyesini yazıp pad'i tutar (hold); açılışta aynı seviye sürülünce bırakılır. Cihazda doğrulandı: açılış IO_MUX `0x0a02` (iç pull yok, yüzer), yeniden başlatma sonrası `0x1802` + tutma var. Güç kesilip gelince ROM→app_main (~300 ms) penceresini yalnız donanım direnci kapatır (aktif-yüksek: pull-down, `inverted`: pull-up) | Kullanıcı şartı: röle enerji kesilip gelince milisaniye bile oynamamalı |
| 2026-11-06 | **K1 değişti:** ayrı yayın deposu `smrtkrft/LebensSpur-firmware` silindi; OTA yayınları ana depo **`smrtkrft/LebensSpur`** (kaynak + yayın tek depo). `project_name` kapısı aynen kalır | Kullanıcı kararı; ayrı depo gerekçesi (ana depoda DMF yayını) DMF yayını silinince kalktı |
| 2026-11-06 | Yayınlar **sıfırdan**: deneme yayınları (`v1.0.1.1`, `v1.0.1.2`) ve ana depodaki eski DMF yayını (`DMF_v1.0.6`) etiketleriyle silindi; **ilk yayın `v1.0.0`** (başlık `LS V1.0.0`; etiket `v` önekli kalır, cihaz sürümü etiketten okur), cihaz sürümü 1.0.0. Sürüm karşılaştırma 4 parçayı desteklemeye devam eder (ara yayın 1.0.0.1) | Kullanıcı kararı; ana depoda eski ürün kodunun yayını kalmasın |
