# Denetim (adım 6) — ara rapor

Tarih: 2026-11-06. Sürüm: 1.0.0.3 (kaynak `code/`, yayınlar `smrtkrft/LebensSpur` → Releases).
Tezgâh: ESP32-C6 rev 0.2, 4 MB dahili + 16 MB harici flash, WiFi "Emek", Mac aynı ağda.

## 1. Test altyapısı

| Katman | Araç | Ne denetler |
|---|---|---|
| 1 | `code/esp_bin/build.sh test` (13 masaüstü testi) | saf C modüller: sayaç, uyarı planı, röle, düğme, outbox, cfg, semver, kanal protokolleri |
| 2 | `build.sh -p PORT flash` | `-Werror` derleme, imaj ≤ 1,5 MiB, cihaza yükleme |
| 3 | `code/test/api/test_api.py` | API sözleşmesi, OTA kapıları, depolama/yedek, röle süre sonu + yeniden başlatma |
| 4 | `code/esp_bin/gui_check.py` (14 adım) | arayüz ekranları, JS/CSP/ağ hataları |
| 5 | `code/esp_bin/gui_roundtrip.py` (26 adım) | arayüz ↔ cihaz her ayar gidiş-dönüş |
| 6 | `code/test/api/test_security.py` (179 kontrol) | parolasız ↔ parolalı karşılaştırma, CSRF, başlıklar, oturum, kilit |
| 7 | `code/test/api/test_stress.py` | istek seli, SSE yuva tükenmesi, yarım bağlantılar, yazma fırtınası, uç değerler, ek sınırları, art arda yeniden başlatma, yazma sırasında reset |
| ayrı | `code/test/api/test_factory.py` + `code/test/gui/backup_offer_steps.json` | fabrika ayarı kademe 1/2, yedek kilidi, geri yükleme / yeni başla, arayüz önerisi |

Kural: her düzeltmeden sonra 1–7 baştan (`code/esp_bin/full_check.sh`); iki tur arka arkaya hatasız olmadan yayın yok.

## 2. Bu denetimde bulunan ve düzeltilen hatalar

| # | Bulgu | Etki | Düzeltme | Doğrulama |
|---|---|---|---|---|
| D1 | Açılışta röle pini polarite okunmadan LOW sürülüyordu | `inverted` rölede her açılışta onlarca ms çekme | pin ayarlar okununca sürülür; `esp_restart` öncesi boşta seviyesi + pad tutma | cihaz: IO_MUX açılış `0x0a02`, yeniden başlatma sonrası `0x1802` + tutma; API testi |
| D2 | Fabrika ayarı sonrası geri yükleme önerisi yoktu, ilk iki kayıt eski yedeği siliyordu | kullanıcı ayarlarını kaybediyordu | yedek kilidi (`meta.flags`), `offer` + `POST /api/backup/dismiss`, arayüz penceresi (6 dil) | `test_factory.py` 0 hata, arayüz ekran görüntüsü |
| D3 | 2 MB ek yüklemesi çip kilidini tüm aktarım boyunca tutuyordu | görev bekçisi cihazı yeniden başlatıyordu (çökme kaydı) | `storage_pin` + parça başına kilit, takılma sayacı ilerlemede sıfır | 2 MB 58 sn, yeniden başlama yok |
| D4 | Ek yüklemesinde öncelik açlığı (httpd 5 > sys_worker 3) | aynı bekçi tetiklenmesi, ağ hızlıyken | parça başına `vTaskDelay(1)`; SMTP ek okumada da | zorlama katmanı tur 20, 21 temiz |
| D5 | SSE: istemci RST ile kapatınca yuva 60 sn dolu | yeni sekme canlı güncelleme alamıyordu | `ECONNRESET` kopma sayılır | zorlama katmanı |
| D6 | Depoda yayın yokken OTA denetimi "beklenmeyen yanıt 302" | anlaşılmaz hata | "yayınlanmış sürüm yok" | API testi |
| D7 | Tatil bitince `vac_total` durumda kalıyordu | kozmetik | tatil sonu/iptalde sıfır | masaüstü testi |
| D8 | Açılışta geçici günlük dökümü (`log_tail`) | gereksiz seri çıktı, açılış 200 ms gecikme | kaldırıldı | derleme |

Önceki adımlarda bulunanlar `00-surec.md` karar günlüğündedir (güvenlik başlıkları, SSE oturum bağı, WiFi yanıt kaybı, zamanlayıcı yarışı, OTA geri alma bilgisi vb.).

## 3. Cihazda gerçek koşullarla doğrulananlar

- Fabrika ayarı kademe 1 ve 2; yedek kilidi; geri yükleme; "yeni başla".
- WiFi yedek ağa geçiş: birincil kaybolunca ~5 sn'de yedeğe, birincil gelince ~5 sn'de geri.
- Ayar yazılırken USB'den sert reset (3 kez): ayarlar, WiFi, yedek bozulmadı.
- Art arda 3 yeniden başlatma; röle her seferinde boşta.
- 8 eşzamanlı istemci × 30 sn (2121 istek, 0 hata); 10 yarım bağlantı; 60 art arda kayıt.
- OTA: denetim, indirme, onay, geri alma, tetiklemede iptal, başka ürün imajının reddi (adım 6); yayınlar `v1.0.0` → `v1.0.0.2`, cihaz güncel.
- Çökme kaydı: cihazdan indirme (`GET /api/coredump`) ve `.elf` ile çözümleme uçtan uca çalışıyor (D3, D4 bu yolla bulundu).

## 4. Açık kalanlar

| Konu | Neden açık | Gereken |
|---|---|---|
| Güç gelişinde ilk ~300 ms röle | ROM→app_main arası pad yüzer (iç pull yok, ölçüldü); yazılım erişemez | kartta sürücü girişinde direnç: aktif-yüksek → pull-down, `inverted` → pull-up (kullanıcı onayı bekleniyor) |
| SMTP (Gmail) ve Telegram gerçek hesap testi | hesap bilgisi yok | uygulama parolası, bot token |
| Harici flash yokken çalışma, gerilim düşmesi | fiziksel müdahale ister | tezgâhta flash'ı ayırma / ayarlı güç kaynağı |
| Eski çökme kaydı (`esp_timer` içinde `abort`) | 1.0.1.x deneme derlemesinden; `.elf` silinmişti, satır eşlenemedi | yeniden görülürse yayın `.elf`'iyle çözülür (artık her yayında `.elf` var) |
| Ek yükleme hızı ~36 KB/s | harici flash yazma hızı; kararlılık sorunu değil | iyileştirme isterse ayrı iş |
| Parolasız cihazda 16 açık uç | tasarım gereği (kurulum kolaylığı, kullanıcı kararı); arayüz kalıcı uyarı gösterir | — |
