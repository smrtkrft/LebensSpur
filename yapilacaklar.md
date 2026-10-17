# LebensSpur — yapılacaklar

Donanım/arayüz hattı: `ALTE/HW` + `ALTE/GUI` (aktif). `Neue/` (sk_core, ortak app) park halinde.
Build: `./esp_bin/build.sh` (tüm çıktılar `esp_bin/` altında).

## GUI'de olup donanımda karşılığı olmayan / eksik

1. **Public WiFi (captive portal).** Akordiyon kaldırıldı; giriş sayfası gerektiren açık ağlara bağlanma
   için genel bir çözüm yok. Yol haritasında.
2. **Webhook/MQTT şablon kütüphanesi.** Webhook ve MQTT çalışıyor; yaygın servisler için hazır
   şablonlar (Home Assistant, ntfy, Pushover...) eklenebilir.
3. **Çoklu cihaz yönetimi.** Tek panelden birden fazla cihaz. Yol haritasında.

## Donanımda olup arayüzde olmayan

4. **Uzaktan sıfırlama API logu.** `GET /api/reset?key=` çağrıları `api` kategorisinde loglanıyor;
   GUI'de ayrı bir "son uzaktan sıfırlama" göstergesi yok.
5. **SNTP sunucusu ayarı.** `pool.ntp.org` sabit; GUI'den değiştirilemiyor.

## Teknik borç / notlar

6. **SMTP sertifika doğrulaması** `MBEDTLS_SSL_VERIFY_OPTIONAL`: geçersiz sertifikada gönderim yine
   yapılır (teslimat önceliği). İstenirse "sertifikayı doğrula" seçeneği eklenebilir.
7. **Harici flash 16 MB:** bench cihazındaki çip 16 MB; bölümler artık çip boyutundan türetiliyor
   (Slot A 2 MB, Slot B 2 MB, user data kalan). 32 MB çipte 28 MB user data.
8. **GUI OTA kaynak yolu** `main/ALTE/GUI/`. Depo yapısı değişirse `gui_ota.c` içindeki
   `DEFAULT_REPO_URL` güncellenmeli. Firmware OTA GitHub Releases `lebensspur.bin` bekler.
9. **Canlı HTTP testi:** bench cihazı router'a (reason 201) ulaşamadığı için API uçları seri log ve
   kod incelemesiyle doğrulandı; router kapsamında cihazla uçtan uca GUI testi yapılmalı
   (giriş ekranı, webhook/MQTT test düğmeleri, statik IP alanları, log görünümü).
