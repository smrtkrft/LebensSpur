# LebensSpur — yapılacaklar

Bu dosya yeniden yapılandırma (kod adı 2611) kapsamı **dışında** bırakılan, ayrı oturumda ele alınacak işleri tutar. Yeniden yapılandırma yeni özellik eklemez; buradakiler sonraya.

## Açık işler — denetim (adım 6), 2026-11-06 itibarıyla

Ayrıntı: `docs/05-denetim.md` §4. Bunlar kapanınca denetim raporu kapanır.

### Kullanıcı tarafında (bilgi ya da fiziksel müdahale gerekiyor)

- [ ] **Röle sürücü direnci:** kartta röle sürücü girişinde boşta seviyesini tutan direnç var mı? Aktif-yüksek bağlantıda GND'ye ~10 kΩ pull-down, `inverted` bağlantıda beslemeye ~10 kΩ pull-up. Güç gelişinde ilk ~300 ms'de (ROM → uygulama) pin yüzer (cihazda ölçüldü, iç pull yok); röleyi bu sürede yalnız bu direnç sabit tutar.
- [ ] **SMTP gerçek hesap testi:** Gmail adresi + uygulama parolası (normal parola çalışmaz).
- [ ] **Telegram gerçek hesap testi:** bot token + sohbet (chat) kimliği.
- [ ] **Harici flash yokken çalışma:** tezgâhta harici flash'ı ayırma (ya da CS hattını kesme) imkânı.
- [ ] **Gerilim düşmesi (brownout):** ayarlı güç kaynağıyla besleme gerilimini düşürme imkânı.

### Yazılım tarafında (bilgi/imkân gelince ya da sırada)

- [ ] Kullanıcıdan gelen bilgilerle yukarıdaki testleri yapmak; sonuçları `05-denetim.md`'ye yazıp raporu kapatmak.
- [ ] Röle direnci yoksa: donanım notu ve arayüzde `inverted` seçeneğinin yanına "harici pull-up gerekir" uyarısı (6 dil).
- [ ] Gmail/Telegram bilgileri yalnız cihaza API ile girilir; kaynağa, git'e, belgelere yazılmaz.
- [ ] Geliştirme kancası (`LS_DEV_WIFI_SSID/PASS`): yalnız derleme ortamından, ürün derlemesinde etkisiz; üretim öncesi kaldırılıp kaldırılmayacağına karar.
- [ ] Eski çökme kaydı (`esp_timer` içinde `abort`, 1.0.1.x deneme derlemesi, `.elf` yok): yeniden görülürse yayının `.elf`'iyle çözmek.
- [ ] İsteğe bağlı iyileştirme: ek yükleme hızı ~36 KB/s (harici flash yazma hızı); kararlılık sorunu değil.

## Ayrı oturum konuları

1. ~~**Alarm sayısı ve alarmlar arası süre yeniden planlanacak.**~~ (2026-11-05) — **karar verildi**: model C, bkz. `docs/06-alarm-tasarim.md`; adım 2.1'de uygulanır.
   Bugünkü model: tek "erken uyarı sayısı" + süreye göre türetilen aralık; değer sessizce yarıya kırpılıyor ve ölçüt saniye değil birim adedi. 2611'de bu mantık **bugünkü haliyle** taşınır, ama sayaç durum makinesinde tek bir "uyarı zamanlaması" fonksiyonuna izole edilir ki sonraki oturumda yalnızca o fonksiyon ve ilgili GUI alanı değişsin. Yeni model (sabit aralık? uyarı başına ayrı zaman? son uyarı tetiklemeden X önce?) o oturumda kararlaştırılacak.

## Haritada görülen, 2611'e girmeyen özellik fikirleri

- Captive portal (AP modunda DNS yakalama) — yeni özellik.
- Webhook/MQTT şablon kütüphanesi (Home Assistant, ntfy, Pushover) — ALTE yapılacaklar listesinden.
- Çoklu cihaz yönetimi — ALTE yapılacaklar listesinden.
- SNTP sunucusu ayarı GUI'den — ALTE yapılacaklar listesinden.
- Uzaktan sıfırlama API logu için GUI göstergesi — ALTE yapılacaklar listesinden.
- Secure Boot v2 + Flash Encryption + NVS şifreleme — üretim öncesi adım; eFuse yakımı bench kartında yapılmaz. 2611 tasarımı bunu engellemeyecek (sırlar tek namespace'te, OTA imzalı imaj formatına hazır).

## Alarm sistemine sonradan eklenebilecekler (06-alarm-tasarim.md)

- Hazır uyarı profilleri ("Rahat / Normal / Sıkı").
- Son uyarıda ikinci bir kişiye haber verme.
- Uyarı mailinde tek tıkla erteleme bağlantısı.
