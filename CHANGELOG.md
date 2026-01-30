# SmartKraft LebensSpur - Changelog

## v1.1.5 (2025-12-02) - TERMAL KORUMA SİSTEMİ

### 🔥 Termal Koruma
- **Dinamik WiFi Yönetimi**: Sıcaklığa göre otomatik WiFi TX gücü ayarı
  - Normal (<70°C): WiFi tam güç (21 dBm)
  - Uyarı (70-85°C): WiFi düşük güç (10 dBm)
  - Kritik (>85°C): WiFi geçici kapalı
- **Histerezis**: 5°C histerezis ile dalgalanma önleme (65°C altında normale dön)
- **Otomatik Kurtarma**: Soğuduğunda WiFi otomatik açılır

### 🌐 Web Arayüzü Sıcaklık Göstergesi
- **Durum Kartı**: Ana sayfada gerçek zamanlı sıcaklık göstergesi
- **Renk Kodlu**: Normal (beyaz), Uyarı (sarı), Kritik (kırmızı)
- **Termal Uyarı**: WiFi kapalıysa "WiFi KAPALI!" uyarısı

### 📡 API Güncellemesi
- `/api/status` yanıtına `thermal` objesi eklendi:
  - `current`: Anlık sıcaklık
  - `min/max`: Min/Max sıcaklıklar
  - `state`: "Normal" / "Uyarı" / "Kritik"
  - `wifiDisabled`: Termal koruma WiFi'yi kapattı mı

---

## v1.1.4 (2025-12-02) - DAHİLİ SICAKLIK SENSÖRÜ

### 🌡️ Sıcaklık İzleme Sistemi
- **ESP32-C6 Dahili Sensör**: Çip sıcaklığı gerçek zamanlı izleniyor
- **Min/Max Takibi**: Görülen minimum ve maksimum sıcaklık kaydediliyor
- **Uyarı Sistemi**:
  - 60°C üzeri → Dikkat uyarısı
  - 70°C üzeri → Kritik uyarı (havalandırma gerekli)

### 📊 Serial Çıktılar
- Başlangıçta: `[TEMP] ✓ Sensör aktif - Başlangıç: XX.X°C`
- Her 5 dakikada: `[HEALTH] Sıcaklık: XX.X°C (Min: XX.X°C, Max: XX.X°C)`
- Setup sonunda: `[SYS] Çip Sıcaklığı: XX.X°C`

### 🔧 Teknik Detaylar
- API: `driver/temperature_sensor.h`
- Ölçüm aralığı: -10°C ~ 80°C (konfigüre edilmiş)
- Doğruluk: ±1°C (tipik)

---

## v1.1.3 (2025-12-02) - GÜÇ KAYNAĞI KORUMASI

### ⚡ Güç Kaynağı İzleme Sistemi
- **ADC Voltaj Ölçümü**: ESP32-C6 dahili ADC ile güç kaynağı voltajı izleniyor
- **Brownout Sayacı**: Tekrarlayan düşük voltaj olayları sayılıyor
- **Voltaj İstatistikleri**: Min/Max/Son voltaj değerleri takip ediliyor
- **Dinamik WiFi TX Gücü**: Düşük voltajda WiFi gücü otomatik düşürülüyor
  - Normal (>4.5V): 21 dBm (maksimum)
  - Düşük (4.3-4.5V): 15 dBm (orta)
  - Kritik (<4.3V): 10 dBm (minimum)

### 🔄 Akıllı Kurtarma
- **Brownout Koruması**: 5 dakikada 5+ brownout = otomatik restart
- **10 Dakika Kuralı**: Uzun süre ağ aktivitesi yoksa restart
- **Boot Reason Logging**: Brownout/Watchdog/Panic kayıt altına alınıyor

### 📊 Sağlık Raporlama
- Her 5 dakikada bir güç durumu loglanıyor:
  - Anlık, minimum ve maksimum voltaj
  - Brownout ve reset sayısı
- Serial çıktı formatı: `[HEALTH] Voltaj: X.XXV (Min: X.XXV, Max: X.XXV) | Brownout: X | Sıfırlama: X`

---

## v1.1.2 (2024-12-02) - KURTARMA MEKANİZMASI

### 🛡️ Kurtarma Özellikleri
- **Boot Reason Logging**: ESP32 reset nedeni Serial'e yazdırılıyor
- **Brownout Uyarısı**: Brownout reset tespit edildiğinde detaylı uyarı
- **Ağ Aktivitesi Takibi**: WiFi/AP bağlantısı yoksa restart sayacı

---

## v1.1.1 (2024-12-02) - HOTFIX

### 🔥 Kritik Düzeltme
- **WiFi Reconnect**: Loop'ta WiFi bağlantı kontrolü eklendi (her 30 saniyede)
- **Sağlık Logu**: 5 dakikada bir uptime, heap, WiFi durumu loglanıyor
- **Watchdog Koruması**: Mail queue işleme öncesi/sonrası watchdog besleme eklendi

### Sorun
- Cihaz 12 saat sonra erişilemez hale geldi
- WiFi koptuğunda otomatik yeniden bağlanma yoktu
- AP moduna da geçmiyordu

---

## v1.1.0 (2024-12-02)

### 🐛 Hata Düzeltmeleri
- **Watchdog Timeout**: `connectToKnown()` içinde uzun WiFi bağlantı döngülerinde watchdog reset eklendi
- **millis() Overflow**: 49 gün sonrası taşma koruması eklendi
- **Duplicate Declarations**: `network_manager.h` içindeki tekrarlanan fonksiyon bildirimleri düzeltildi
- **ScheduleSnapshot Fields**: Mail fonksiyonlarında eksik alanlar için boş string kullanıldı

### 🆔 Benzersiz Cihaz ID Sistemi
- **Sorun**: Çinli klon ESP32-C6 modülleri aynı MAC adresiyle geliyor
- **Çözüm**: NVS + LittleFS hibrit ID sistemi
  - NVS (Non-Volatile Storage): Flash silinse bile korunur
  - LittleFS: Yedek olarak saklanır
  - ID oluşturma: MAC + TRNG + micros() kombinasyonu
- **Kalıcılık**: OTA/USB upload ile değişmez, sadece "Erase All Flash" ile silinir

### 📡 OTA Güncelleme Sistemi
- Ayrı `ota_manager.h/cpp` modülü oluşturuldu
- 24-48 saat arası random kontrol aralığı (rate limit koruması)
- İlk açılışta 1-5 dakika sonra kontrol
- Durum LittleFS'te kalıcı olarak saklanır

### 🔧 Diğer İyileştirmeler
- Chip ID artık tam 12 karakter gösteriliyor
- AP adı formatı: `LS-XXXXXXXXXXXX`
- mDNS formatı: `ls-xxxxxxxxxxxx.local`
- Status API'ye `macAddress` alanı eklendi

---

## 📋 Yapılacaklar (TODO)

### 🎯 Öncelikli
- [ ] **OTA Sistemi**: Kullanıcı opsiyonel - Otomatik/Manuel seçeneği
- [ ] **Fiziksel Testler**: Tüm cihaz fonksiyonlarının gerçek ortamda testi

### 🎨 Görsel Tasarım
- [ ] **Buton Taşıma**: Reboot ve Factory Reset butonları Info sekmesine taşınacak
- [ ] **Ağ Sekmesi**: WiFi ayarları sayfası yeniden tasarlanacak
- [ ] **Info Sekmesi Genişletme**:
  - OTA durum bilgisi ve son kontrol zamanı
  - "Yeni Sürüm" bilgilendirme alanı
  - Changelog/Release Notes görüntüleme

### 📝 Notlar
- ESP32-C6 için tüm uyku modları devre dışı (web server için gerekli)
- Watchdog: 30 saniye timeout
- Heap kritik seviye: 20KB
- Periyodik restart: 24 saat

---

## 📊 Versiyon Geçmişi

| Versiyon | Tarih | Açıklama |
|----------|-------|----------|
| v1.1.0 | 2024-12-02 | NVS ID sistemi, OTA modülü, hata düzeltmeleri |
| v1.0.6 | 2024-11-xx | Mail queue sistemi, WiFi fallback |
| v1.0.5 | 2024-11-xx | İlk kararlı sürüm |
