# 2611 — yapılacaklar

Bu dosya 2611 kapsamı **dışında** bırakılan, ayrı oturumda ele alınacak işleri tutar. 2611 yeni özellik eklemez; buradakiler sonraya.

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
