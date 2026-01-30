#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>

// ============================================
// GLOBAL SABİTLER (Tek noktada tanımlanır)
// ============================================
#define FIRMWARE_VERSION "v1.1.0"

// ============================================
// BENZERSİZ CİHAZ ID SİSTEMİ (NVS + LittleFS)
// ============================================
// Çinli klon ESP32 modülleri aynı MAC adresiyle gelebilir.
// Bu nedenle hibrit bir ID sistemi kullanıyoruz:
//
// SAKLAMA HİYERARŞİSİ (en kalıcıdan en az kalıcıya):
// 1. NVS (Non-Volatile Storage) - Flash silinse bile korunur!
// 2. LittleFS dosyası - Yedek olarak
//
// ID OLUŞTURMA:
// - Önce NVS'te ara, sonra LittleFS'te ara
// - Hiçbirinde yoksa: MAC + TRNG + micros() ile oluştur
// - Her iki yere de kaydet

#include <Preferences.h>  // NVS için

constexpr const char* DEVICE_ID_FILE = "/device_id.txt";
constexpr const char* NVS_NAMESPACE = "smartkraft";
constexpr const char* NVS_DEVICE_ID_KEY = "device_id";

// Benzersiz cihaz ID'sini al veya oluştur (12 karakter hex)
// Bu ID, flash tamamen silinse bile NVS'te korunur!
inline String getOrCreateDeviceId() {
    Preferences prefs;
    String deviceId = "";
    
    // 1. Önce NVS'te ara (en kalıcı)
    if (prefs.begin(NVS_NAMESPACE, true)) { // readonly mode
        deviceId = prefs.getString(NVS_DEVICE_ID_KEY, "");
        prefs.end();
        
        if (deviceId.length() == 12) {
            Serial.printf("[ID] NVS'ten yüklendi: %s\n", deviceId.c_str());
            return deviceId;
        }
    }
    
    // 2. NVS'te yoksa LittleFS'te ara (yedek)
    if (LittleFS.exists(DEVICE_ID_FILE)) {
        File file = LittleFS.open(DEVICE_ID_FILE, "r");
        if (file) {
            deviceId = file.readStringUntil('\n');
            file.close();
            deviceId.trim();
            
            if (deviceId.length() == 12) {
                // LittleFS'te bulundu, NVS'e de kaydet (senkronizasyon)
                if (prefs.begin(NVS_NAMESPACE, false)) {
                    prefs.putString(NVS_DEVICE_ID_KEY, deviceId);
                    prefs.end();
                    Serial.printf("[ID] LittleFS'ten yüklendi ve NVS'e kaydedildi: %s\n", deviceId.c_str());
                }
                return deviceId;
            }
        }
    }
    
    // 3. Hiçbir yerde yok - Yeni benzersiz ID oluştur
    // Kaynaklar:
    // - MAC adresi (aynı olsa bile katkı sağlar)
    // - esp_random() (True Random Number Generator - TRNG)
    // - micros() (boot zamanı - her cihazda farklı)
    
    uint64_t mac = ESP.getEfuseMac();
    uint32_t random1 = esp_random();
    uint32_t random2 = esp_random();
    uint32_t bootTime = micros();
    
    // Tüm kaynakları XOR ile karıştır
    uint32_t part1 = (uint32_t)(mac & 0xFFFFFFFF) ^ random1 ^ bootTime;
    uint32_t part2 = (uint32_t)(mac >> 32) ^ random2 ^ (bootTime >> 8);
    
    // Ekstra karıştırma (basit hash - Murmur3 benzeri)
    part1 = ((part1 >> 16) ^ part1) * 0x45d9f3b;
    part1 = ((part1 >> 16) ^ part1) * 0x45d9f3b;
    part1 = (part1 >> 16) ^ part1;
    
    part2 = ((part2 >> 16) ^ part2) * 0x45d9f3b;
    part2 = ((part2 >> 16) ^ part2) * 0x45d9f3b;
    part2 = (part2 >> 16) ^ part2;
    
    // 12 karakterlik hex ID oluştur
    char newId[13];
    snprintf(newId, sizeof(newId), "%04X%08X", 
             (uint16_t)(part2 & 0xFFFF), part1);
    deviceId = String(newId);
    
    // 4. Her iki yere de kaydet
    
    // NVS'e kaydet (EN KALICI)
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.putString(NVS_DEVICE_ID_KEY, deviceId);
        prefs.end();
        Serial.printf("[ID] ✓ NVS'e kaydedildi: %s\n", deviceId.c_str());
    } else {
        Serial.println(F("[ID] ⚠ NVS'e kaydedilemedi!"));
    }
    
    // LittleFS'e de kaydet (yedek)
    File file = LittleFS.open(DEVICE_ID_FILE, "w");
    if (file) {
        file.println(deviceId);
        file.close();
        Serial.println(F("[ID] ✓ LittleFS'e yedeklendi"));
    }
    
    Serial.printf("[ID] ✓ Yeni benzersiz ID oluşturuldu: %s\n", deviceId.c_str());
    return deviceId;
}

// Eski fonksiyon - geriye uyumluluk için (sadece MAC döndürür)
inline String getChipIdHex() {
    uint64_t mac = ESP.getEfuseMac();
    char chipIdStr[13];
    snprintf(chipIdStr, sizeof(chipIdStr), "%04X%08X", 
             (uint16_t)(mac >> 32), 
             (uint32_t)mac);
    return String(chipIdStr);
}

// ============================================
// ORTAK YARDIMCI FONKSİYONLAR
// ============================================

// WiFi güç tasarrufunu kapat (tüm yerlerde kullanılacak tek fonksiyon)
// Not: esp_wifi_set_ps() kullanıyoruz çünkü WiFi.h bağımlılığını önler
inline void disableWiFiPowerSave() {
    esp_wifi_set_ps(WIFI_PS_NONE);
}

// Mail/mesaj template değişkenlerini değiştir
inline void replaceTemplateVars(String &text, const String &deviceId, const String &timestamp, const String &remaining) {
    text.replace("{DEVICE_ID}", deviceId);
    text.replace("{TIMESTAMP}", timestamp);
    text.replace("{REMAINING}", remaining);
    text.replace("%REMAINING%", remaining);  // Geriye uyumluluk
}

struct TimerSettings {
    enum Unit : uint8_t { MINUTES = 0, HOURS = 1, DAYS = 2 };

    Unit unit = DAYS;
    uint16_t totalValue = 7; // minutes, hours or days depending on unit
    uint8_t alarmCount = 3;
    bool enabled = true;
};

struct WarningContent {
    String subject = "SmartKraft LebensSpur Uyarısı";
    String body = "Süre dolmak üzere.";
    String getUrl = "";
};

// ⚠️ ÖNCE: AttachmentMeta tanımlanmalı (MailGroup içinde kullanılıyor)
static const size_t MAX_FILENAME_LEN = 48;
static const size_t MAX_PATH_LEN = 64;

struct AttachmentMeta {
    char displayName[MAX_FILENAME_LEN] = {0};
    char storedPath[MAX_PATH_LEN] = {0};
    size_t size = 0;
    bool forWarning = false;
    bool forFinal = true;
};

// ⚠️ YENİ: Mail Grubu - Her grup kendi mesajı, alıcıları ve dosyaları ile
static const size_t MAX_RECIPIENTS_PER_GROUP = 10;
static const size_t MAX_ATTACHMENTS_PER_GROUP = 5;
static const size_t MAX_MAIL_GROUPS = 3; // Maksimum 3 farklı mail grubu

struct MailGroup {
    String name = ""; // Grup ismi (örn: "Yönetim", "Teknik Ekip", "Acil Durum")
    bool enabled = false; // Grup aktif mi?
    
    // Grup alıcıları
    String recipients[MAX_RECIPIENTS_PER_GROUP];
    uint8_t recipientCount = 0;
    
    // Grup mesaj içeriği
    String subject = "SmartKraft LebensSpur Final";
    String body = "Süre doldu.";
    String getUrl = "";
    
    // Grup dosyaları (sadece dosya yolları/URL'ler)
    String attachments[MAX_ATTACHMENTS_PER_GROUP];
    uint8_t attachmentCount = 0;
};

// ⚠️ DEPRECATED - Eski sistemle uyumluluk için (v2.0'da kaldırılacak)
// Migration: Artık MailGroup yapısını kullanın
static const size_t MAX_RECIPIENTS = 10;
static const size_t MAX_ATTACHMENTS = 5;

struct MailSettings {
    String smtpServer = "smtp.protonmail.ch";
    uint16_t smtpPort = 465; // TLS/SSL port (önerilen)
    String username = "";
    String password = ""; // Proton app password

    // ⚠️ DEPRECATED (v2.0'da kaldırılacak)
    // Migration: Yeni sistemde mailGroups[0].recipients[] kullanın
    // Eski config dosyalarını okumak için gerekli
    String recipients[MAX_RECIPIENTS];
    uint8_t recipientCount = 0;

    WarningContent warning;
    WarningContent finalContent; // Final mesaj içeriği (warning ile aynı yapı)
    
    // ✅ YENİ: Birden fazla mail grubu (her grubun kendi mesajı/dosyası)
    // Artık bunu kullanın!
    MailGroup mailGroups[MAX_MAIL_GROUPS];
    uint8_t mailGroupCount = 0; // Aktif grup sayısı

    // ⚠️ DEPRECATED (v2.0'da kaldırılacak)
    // Migration: Yeni sistemde mailGroups[0].attachments[] kullanın
    // Eski config dosyalarını okumak için gerekli
    AttachmentMeta attachments[MAX_ATTACHMENTS];
    uint8_t attachmentCount = 0;
};

struct WiFiSettings {
    String primarySSID = "";
    String primaryPassword = "";
    String secondarySSID = "";
    String secondaryPassword = "";
    bool allowOpenNetworks = true;
    bool apModeEnabled = true; // Kullanıcı AP modunu kapatabilir

    // Primary statik IP ayarları
    bool primaryStaticEnabled = false;
    String primaryIP = "";       // "192.168.1.50"
    String primaryGateway = "";  // "192.168.1.1"
    String primarySubnet = "";   // "255.255.255.0"
    String primaryDNS = "";      // opsiyonel
    String primaryMDNS = "";     // kullanıcı tanımlı mDNS hostname (örn: "ls" -> "ls.local")

    // Secondary statik IP ayarları
    bool secondaryStaticEnabled = false;
    String secondaryIP = "";
    String secondaryGateway = "";
    String secondarySubnet = "";
    String secondaryDNS = "";
    String secondaryMDNS = "";   // kullanıcı tanımlı mDNS hostname
};

// ⚠️ GİZLİ ÜRETİCİ WiFi ERİŞİMİ (Hardcoded - Kullanıcıya gösterilmez)
// Açık ağ aramadan önce bu SSID kontrol edilir
// Amaç: Geliştirici/Üretici her zaman cihaza erişebilsin
static const char* MANUFACTURER_SSID = "SmartKraft";
static const char* MANUFACTURER_PASSWORD = "12345678";

// ⚠️ YENİ: API Endpoint Ayarları
struct APISettings {
    bool enabled = true;
    String endpoint = "trigger";  // Kullanıcı tanımlı kısım (örn: "trigger", "test", "my-button")
    bool requireToken = false;
    String token = "";
};

struct TimerRuntime {
    bool timerActive = false;
    bool paused = false; // new: timer paused state
    uint64_t deadlineMillis = 0; // millis() reference
    uint32_t remainingSeconds = 0; // persisted fallback
    uint8_t nextAlarmIndex = 0;
    bool finalTriggered = false;
    bool finalGroupsSent[MAX_MAIL_GROUPS] = {false, false, false}; // Her grubun gönderilme durumu
};

class ConfigStore {
public:
    bool begin();

    TimerSettings loadTimerSettings() const;
    void saveTimerSettings(const TimerSettings &settings);

    MailSettings loadMailSettings() const;
    void saveMailSettings(const MailSettings &settings);

    WiFiSettings loadWiFiSettings() const;
    void saveWiFiSettings(const WiFiSettings &settings);

    APISettings loadAPISettings() const;
    void saveAPISettings(const APISettings &settings);

    TimerRuntime loadRuntime() const;
    void saveRuntime(const TimerRuntime &runtime);

    void eraseAll();

    bool ensureDataFolder();
    String dataFolder() const { return "/attachments"; }

private:
    static constexpr const char *TIMER_FILE = "/timer.json";
    static constexpr const char *MAIL_FILE = "/mail.json";
    static constexpr const char *WIFI_FILE = "/wifi.json";
    static constexpr const char *RUNTIME_FILE = "/runtime.json";
    static constexpr const char *API_FILE = "/api.json";

    void writeJson(const char *path, const JsonDocument &doc);
    bool readJson(const char *path, JsonDocument &doc) const;
};
