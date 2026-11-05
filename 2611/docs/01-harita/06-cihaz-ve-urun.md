# 06 — Cihaz ve ürün düzeyi harita

Kaynaklar: `ALTE/README.md`, `ALTE/HW/components/output/output.c` pin tanımları, bench notları (2026-10-17), git geçmişi (236 commit), `GUVENLIK_BULGUSU_WEBHOOK_AUTH.md`. Kod düzeyindeki ayrıntılar 01–05 belgelerinde.

## Ürün: ne yapar

LebensSpur bir "ölü adam anahtarı" (dead man's switch). Kullanıcı ayarlanan süre içinde fiziksel ya da sanal düğmeyle **erteleme (check-in)** yapmazsa:

1. **Erken uyarı**: süre dolmadan önce, ayarlanan sayıda uyarı e-postası kullanıcının kendi adresine gider (webhook/MQTT de bildirilir).
2. **Protokol tetiklenmesi**: süre dolunca kayıtlı kişi/gruplara önceden hazırlanmış mesajlar gider (e-posta, Telegram, webhook/MQTT), röle çekilir.

Hedef kullanıcılar: yalnız yaşayan yaşlılar, kronik hastalar, kritik bilgi sahipleri, acil durum planı yapanlar. Güvenlik ürünü: **teslimat önceliklidir** (ör. SMTP sertifika doğrulaması bilerek "isteğe bağlı").

## Kullanıcıya görünen özellikler (README'den)

| Özellik | Not |
|---|---|
| Sayaç | 1 dakika – 60 gün |
| Erken uyarı | istenen sayıda, kullanıcının kendi adresine |
| E-posta | SMTP; ProtonMail, Gmail, Outlook, özel sunucu; 3 alıcı grubu |
| Telegram | bot ile mesaj |
| Webhook / MQTT | erken uyarı ve tetiklenmede dış sistemlere bildirim |
| Düğme | fiziksel (karttaki) + sanal (web) + API ucu (`/api/reset?key=`, mobil entegrasyon) |
| Röle | tetiklenmede çekilir; en çok 5 V 30 mA (aşılırsa cihaz zarar görür) |
| Web paneli | tüm ayarlar; sekmeler: ana sayfa, sayaç, mail, WiFi, API, test |
| WiFi | üç kademeli altyapı (birincil/ikincil STA, sonra AP); statik IP; TX güç yönetimi |
| İlk kurulum | AP "SmartKraft-LebensSpur", `http://192.168.4.1` |
| OTA | GitHub Releases üzerinden firmware; GitHub raw üzerinden GUI (bu ikincisi 2611'de kalkıyor) |
| Dil | TR, EN, DE, ES, FR, IT |
| Olay günlüğü | kalıcı; kapanış nedeni sınıflandırması (brownout / güç kesintisi / çökme / kullanıcı), check-in'ler, mailler, tetiklenmeler |
| Tarayıcı parolası | oturum çerezi, başarısız denemede kilit |
| Lisans | AGPL-3.0; SmartKraft açık kaynak topluluğu; geliştirici SEU // Emek Ulaş Suna |

## Donanım

| Öğe | Değer |
|---|---|
| Kart | Seeed XIAO ESP32-C6 (RISC-V 160 MHz, WiFi 2.4 GHz b/g/n, BLE var ama kullanılmıyor) |
| Dahili flash | 4 MB; iki OTA uygulama bölümü (`ota_0`, `ota_1` 0x1F0000 = 1984 KB), NVS 24 KB, otadata, phy_init |
| Harici flash | SPI, çip boyutu açılışta okunuyor; bench kartında **16 MB** (kod bir dönem 32 MB W25Q256 varsayıyordu) |
| Harici flash pinleri | CS=GPIO21 (D3), MISO=GPIO0 (D0), MOSI=GPIO22 (D4), SCLK=GPIO16 (D6) |
| Düğme | GPIO18 (D10), aktif LOW, dahili pull-up |
| Röle | GPIO19 (D8), aktif HIGH; GPIO19–23 C6'da SDIO'ya atanmış, önce `gpio_reset_pin` gerekir |
| Konsol | USB Serial/JTAG (`/dev/cu.usbmodem1101`). UART0 TX/RX = GPIO16/17 harici flash SCLK ile çakıştığı için UART konsolu **kapalı** |
| Besleme | 5 V USB-C ya da 230 V AC (kart üstü dönüştürücü) |

### Besleme geçmişi (bench notları)

PCB rev 1 sürekli dondu/resetlendi; rev 2 yaklaşık 72 saatte bir yeniden başladı; rev 3 12 gün çalıştı, sonra bir telefon AP'ye katıldıktan ~15 s sonra resetlendi. Bu yüzden:

- brownout dedektörü açık, `sys_log` açılış kaydında neden sınıflandırması var,
- WiFi TX gücü ayarlanabilir/adaptif,
- GUI'nin gzip'lenmesi "WiFi TX süresi kısalsın, akım pikleri düşsün" gerekçesiyle yapıldı.

2611'de bu kısıt **tasarım girdisi**: ağ üzerinde gönderilen byte ve eşzamanlı TX süresi düşük tutulmalı; büyük sayfalar, sık polling, gereksiz eşzamanlı bağlantılar istenmiyor.

## Bench cihazı

- Cihaz adı `LS-1EKK63U5BA`, port `/dev/cu.usbmodem1101`.
- Kayıtlı birincil WiFi "Emek" bench'ten erişilemiyor (reason 201) → cihaz AP modunda kalıyor; LAN üzerinden uçtan uca GUI testi henüz yapılmadı. 2611 denetiminde bu testin router kapsamında yapılması gerekir.

## Güvenlik bulgusu (2026-08-03, düzeltildi 2026-08-13)

`GUVENLIK_BULGUSU_WEBHOOK_AUTH.md`: eski ağaçtaki USB CLI'da uzaktan sıfırlama anahtarını üreten komutlar yetki kapısı taşımıyordu; birkaç saniyelik fiziksel erişimle kalıcı bir uzaktan "sayacı sıfırla" yetkisi elde edilebiliyordu. Ders: **sayacı sıfırlayabilen her yol** (düğme, web, API anahtarı, ileride CLI) ürünün varlık sebebine dokunur; 2611'de bu yollar tek yerde toplanıp her biri açıkça yetkilendirilmeli, anahtar üretimi/değişimi loglanmalı.

## Git geçmişinden öğrenilenler

- 2026-02: yoğun GUI/web_server geliştirmesi, `session_auth.c` (ALTE'de artık yok; oturum mantığı web_server'a gömülü).
- 2026-06 – 08: `Neue/` (sk_core, SKAPP, USB CLI, BLE eşleştirme) denemesi; park edildi.
- 2026-10-17: ALTE'ye dönüş; tek günde 11 commit: esp_bin derleme düzeni, sys_log, GUI olay günlüğü, WiFi TX gücü, 16 MB flash düzeltmesi, kod analizi düzeltmeleri, rota tablosu, webhook/MQTT gerçek hale getirildi, 6 dil, gzip GUI, boyut küçültme (1.83 → 1.27 MB).
- Sürüm 0.0.70; sürüm üç yerde tutuluyor (CMakeLists `PROJECT_VER`, `ext_flash.c` `WEB_FILES_VERSION`, `GUI/config.json`).

## Firmware boyut bilançosu (v0.0.70)

| Parça | Boyut |
|---|---|
| İmaj | 1.268.896 B (bölüm 1984 KB, %38 boş) |
| Gömülü GUI (gzip) | ~113 KB |
| WiFi kütüphaneleri | ~420 KB |
| mbedTLS | ~210 KB |
| lwIP | ~130 KB |
| Proje kodu | ~70 KB |

Bilerek açık bırakılanlar (yeniden önerme): WPA3/SAE, assert ve mbedTLS hata metinleri, `esp_err_to_name`, tam printf.

## 2611 için çıkarımlar

1. Program yalnızca dahili flash'ta: 1984 KB'lık uygulama bölümü yeterli; hedef 1.5 MiB altı, bugün ~1.27 MB.
2. Harici flash yalnızca veri: tek LittleFS bölümü (ya da birkaç mantıksal dizin), yapılandırma yedeği, olay günlüğü, kullanıcı dosyaları (ileride e-posta ekleri).
3. Tek sürüm numarası, tek OTA kanalı.
4. Güç kısıtı tasarım girdisi: az byte, az eşzamanlı TX.
5. Sayacı sıfırlayan yollar tek kapıdan geçmeli ve loglanmalı.
