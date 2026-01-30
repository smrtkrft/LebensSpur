/**
 * ota_manager.cpp - OTA (Over-The-Air) Güncelleme Yöneticisi
 * 
 * Copyright (c) 2024-2025 SmartKraft
 */

#include "ota_manager.h"
#include <esp_task_wdt.h>

// ============================================
// Constructor
// ============================================

OTAManager::OTAManager() 
    : wifiConnected(false)
    , initialized(false)
    , bootTime(0)
    , lastLoopTime(0)
{
    memset(&state, 0, sizeof(state));
}

// ============================================
// Public Methods
// ============================================

void OTAManager::begin(const String& firmwareVersion) {
    currentVersion = firmwareVersion;
    bootTime = millis();
    lastLoopTime = bootTime;
    
    // Durumu yükle
    loadState();
    
    // İlk açılış kontrolü yapılmadıysa, kısa bir süre sonra kontrol et
    if (!state.startupCheckDone) {
        state.nextCheckInterval = calculateStartupInterval();
        state.lastCheckTime = bootTime;
        Serial.printf("[OTA] İlk açılış - %lu saniye sonra kontrol edilecek\n", 
                      state.nextCheckInterval / 1000);
    } else {
        // Startup kontrolü zaten yapıldı, otomatik kontrol yok
        // Kullanıcı manuel olarak kontrol edebilir
        state.nextCheckInterval = 0; // Otomatik kontrol devre dışı
        Serial.println(F("[OTA] Otomatik kontrol kapalı - Manuel kontrol için web arayüzünü kullanın"));
    }
    
    initialized = true;
    Serial.printf("[OTA] ✓ OTA Manager başlatıldı (v%s)\n", currentVersion.c_str());
    Serial.printf("[OTA] ℹ Toplam kontrol: %lu, Başarılı: %lu, Başarısız: %lu\n",
                  state.checkCount, state.successCount, state.failCount);
}

void OTAManager::loop() {
    if (!initialized) return;
    
    unsigned long now = millis();
    
    // millis() overflow kontrolü
    if (now < lastLoopTime) {
        // Taşma oldu, lastCheckTime'ı sıfırla
        state.lastCheckTime = now;
        Serial.println(F("[OTA] millis() overflow tespit edildi, zamanlama sıfırlandı"));
    }
    lastLoopTime = now;
    
    // Sadece ilk açılış kontrolü (bir kere)
    // Sonrasında kullanıcı manuel kontrol eder
    if (!state.startupCheckDone && state.nextCheckInterval > 0) {
        unsigned long elapsed = now - state.lastCheckTime;
        
        if (elapsed >= state.nextCheckInterval) {
            // WiFi bağlı mı?
            if (wifiConnected) {
                Serial.println(F("[OTA] ⏰ İlk açılış OTA kontrolü başlatılıyor..."));
                
                bool updated = checkForUpdate();
                
                // İlk açılış kontrolü tamamlandı
                state.startupCheckDone = true;
                state.nextCheckInterval = 0; // Otomatik kontrol devre dışı
                
                Serial.println(F("[OTA] Otomatik kontrol tamamlandı. Sonraki güncelleme için web arayüzünü kullanın."));
                
                // Durumu kaydet
                saveState();
                
                // Güncelleme başarılıysa cihaz zaten yeniden başlayacak
                (void)updated;
            } else {
                // WiFi yok, 1 dakika sonra tekrar dene
                state.lastCheckTime = now;
                state.nextCheckInterval = 60UL * 1000UL; // 1 dakika
                Serial.println(F("[OTA] WiFi bağlı değil, 1 dakika sonra tekrar denenecek"));
            }
        }
    }
    // NOT: Periyodik 24-48 saat kontrolü KALDIRILDI
    // Kullanıcı checkForUpdate() fonksiyonunu web arayüzünden manuel olarak çağırır
}

bool OTAManager::checkForUpdate() {
    if (!wifiConnected) {
        Serial.println(F("[OTA] WiFi bağlı değil, kontrol atlandı"));
        return false;
    }
    
    // Watchdog'u besle (uzun işlem)
    esp_task_wdt_reset();
    
    state.checkCount++;
    
    // En son versiyonu al
    String latestVersion;
    if (!fetchLatestVersion(latestVersion)) {
        state.failCount++;
        saveState();
        return false;
    }
    
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    Serial.printf("[OTA] Mevcut: %s, En son: %s\n", 
                  currentVersion.c_str(), latestVersion.c_str());
    
    // Versiyon karşılaştır
    int cmp = compareVersions(currentVersion, latestVersion);
    
    if (cmp < 0) {
        // Güncelleme mevcut
        Serial.println(F("[OTA] ✓ Yeni versiyon bulundu!"));
        
        if (downloadAndUpdate(latestVersion)) {
            state.successCount++;
            saveState();
            // Cihaz yeniden başlayacak, buraya ulaşılmaz
            return true;
        } else {
            state.failCount++;
            saveState();
            return false;
        }
    } else if (cmp > 0) {
        Serial.println(F("[OTA] ℹ Mevcut versiyon daha yeni (dev build)"));
        return false;
    } else {
        Serial.println(F("[OTA] ✓ En güncel versiyondasınız"));
        return false;
    }
}

uint32_t OTAManager::getTimeToNextCheck() const {
    unsigned long now = millis();
    unsigned long elapsed = now - state.lastCheckTime;
    
    if (elapsed >= state.nextCheckInterval) {
        return 0;
    }
    
    return state.nextCheckInterval - elapsed;
}

// ============================================
// Private Methods
// ============================================

uint32_t OTAManager::calculateStartupInterval() {
    // İlk açılışta 1-5 dakika arası random bekle
    // Bu sayede cihaz başlatma işlemleri tamamlanır
    
    uint32_t interval = OTA_STARTUP_MIN_MS + (esp_random() % (OTA_STARTUP_MAX_MS - OTA_STARTUP_MIN_MS));
    
    Serial.printf("[OTA] Startup aralığı: %lu saniye\n", interval / 1000);
    
    return interval;
}

// NOT: calculateNextInterval() KALDIRILDI - periyodik otomatik kontrol yok

void OTAManager::saveState() {
    JsonDocument doc;
    
    doc["nextCheckInterval"] = state.nextCheckInterval;
    doc["checkCount"] = state.checkCount;
    doc["successCount"] = state.successCount;
    doc["failCount"] = state.failCount;
    doc["startupCheckDone"] = state.startupCheckDone;
    
    File file = LittleFS.open(OTA_STATE_FILE, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
    }
}

void OTAManager::loadState() {
    File file = LittleFS.open(OTA_STATE_FILE, "r");
    if (!file) {
        // Dosya yok, varsayılan değerler kullan
        memset(&state, 0, sizeof(state));
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("[OTA] State dosyası parse hatası: %s\n", error.c_str());
        memset(&state, 0, sizeof(state));
        return;
    }
    
    state.nextCheckInterval = doc["nextCheckInterval"] | 0;
    state.checkCount = doc["checkCount"] | 0;
    state.successCount = doc["successCount"] | 0;
    state.failCount = doc["failCount"] | 0;
    state.startupCheckDone = doc["startupCheckDone"] | false;
}

bool OTAManager::fetchLatestVersion(String& latestVersion) {
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    // DNS kontrolü
    Serial.println(F("[OTA] DNS çözümleniyor..."));
    IPAddress testIP;
    if (WiFi.hostByName("api.github.com", testIP) != 1) {
        Serial.println(F("[OTA] ✗ DNS hatası: api.github.com çözümlenemedi"));
        return false;
    }
    Serial.printf("[OTA] ✓ DNS: api.github.com → %s\n", testIP.toString().c_str());
    
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    WiFiClientSecure client;
    client.setInsecure(); // Sertifika doğrulamasını atla
    
    HTTPClient http;
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);
    
    Serial.println(F("[OTA] GitHub API sorgulanıyor..."));
    http.begin(client, OTA_GITHUB_API_URL);
    http.addHeader("User-Agent", "SmartKraft-LebensSpur");
    http.addHeader("Accept", "application/vnd.github.v3+json");
    
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        http.end();
        
        // Watchdog'u besle
        esp_task_wdt_reset();
        
        // JSON'dan tag_name çıkar
        int tagStart = payload.indexOf("\"tag_name\":\"") + 12;
        if (tagStart > 11) {
            int tagEnd = payload.indexOf("\"", tagStart);
            if (tagEnd > tagStart) {
                latestVersion = payload.substring(tagStart, tagEnd);
                latestVersion.trim();
                return true;
            }
        }
        
        Serial.println(F("[OTA] JSON parse hatası"));
        return false;
    }
    
    // Hata durumları
    if (httpCode == 403) {
        Serial.println(F("[OTA] ✗ GitHub API rate limit (403)"));
        Serial.println(F("[OTA] ℹ 1 saat sonra tekrar denenecek"));
    } else if (httpCode == 404) {
        Serial.println(F("[OTA] ✗ Release bulunamadı (404)"));
    } else if (httpCode == -1) {
        Serial.println(F("[OTA] ✗ Bağlantı hatası"));
    } else if (httpCode == -11) {
        Serial.println(F("[OTA] ✗ Timeout"));
    } else {
        Serial.printf("[OTA] ✗ HTTP hatası: %d\n", httpCode);
    }
    
    http.end();
    return false;
}

int OTAManager::compareVersions(const String& current, const String& latest) {
    // Versiyon formatı: "1.2.3" veya "v1.2.3"
    String v1 = current;
    String v2 = latest;
    
    // 'v' prefix'ini kaldır
    if (v1.startsWith("v") || v1.startsWith("V")) v1 = v1.substring(1);
    if (v2.startsWith("v") || v2.startsWith("V")) v2 = v2.substring(1);
    
    // Major.Minor.Patch olarak ayır ve karşılaştır
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    sscanf(v1.c_str(), "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2.c_str(), "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return (major1 < major2) ? -1 : 1;
    if (minor1 != minor2) return (minor1 < minor2) ? -1 : 1;
    if (patch1 != patch2) return (patch1 < patch2) ? -1 : 1;
    
    return 0;
}

bool OTAManager::downloadAndUpdate(const String& version) {
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    WiFiClientSecure client;
    client.setInsecure();
    
    HTTPClient http;
    http.setTimeout(OTA_DOWNLOAD_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    // Firmware URL oluştur
    String firmwareURL = String(OTA_GITHUB_REPO_BASE) + version + "/" + OTA_FIRMWARE_FILENAME;
    
    Serial.printf("[OTA] Firmware indiriliyor: %s\n", firmwareURL.c_str());
    http.begin(client, firmwareURL);
    
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    int httpCode = http.GET();
    
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[OTA] ✗ Firmware indirme hatası: %d\n", httpCode);
        http.end();
        return false;
    }
    
    int contentLength = http.getSize();
    
    if (contentLength <= 0) {
        Serial.println(F("[OTA] ✗ Geçersiz içerik boyutu"));
        http.end();
        return false;
    }
    
    Serial.printf("[OTA] Firmware boyutu: %d bytes\n", contentLength);
    
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    if (!Update.begin(contentLength)) {
        Serial.println(F("[OTA] ✗ Yeterli alan yok"));
        http.end();
        return false;
    }
    
    WiFiClient* stream = http.getStreamPtr();
    
    Serial.println(F("[OTA] Güncelleme yazılıyor..."));
    
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    size_t written = Update.writeStream(*stream);
    
    // Watchdog'u besle
    esp_task_wdt_reset();
    
    if (written != contentLength) {
        Serial.printf("[OTA] ✗ Yazma hatası: %d/%d byte\n", written, contentLength);
        Update.abort();
        http.end();
        return false;
    }
    
    if (!Update.end()) {
        Serial.printf("[OTA] ✗ Update hatası: %s\n", Update.errorString());
        http.end();
        return false;
    }
    
    if (!Update.isFinished()) {
        Serial.println(F("[OTA] ✗ Güncelleme tamamlanamadı"));
        http.end();
        return false;
    }
    
    Serial.println(F("[OTA] ✓ Güncelleme başarılı!"));
    Serial.println(F("[OTA] Yeniden başlatılıyor..."));
    
    http.end();
    delay(1000);
    ESP.restart();
    
    return true; // Buraya ulaşılmaz
}
