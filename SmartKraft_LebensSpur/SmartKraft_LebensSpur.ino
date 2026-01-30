#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include <DNSServer.h>
#include <esp_pm.h>        // Güç yönetimi için
#include <esp_wifi.h>      // WiFi güç yönetimi için
#include <esp_sleep.h>     // Uyku modu kontrolü için
#include <esp_task_wdt.h>  // Watchdog Timer için
// NOT: Termal sıcaklık sensörü sistemi KALDIRILDI (gereksiz)

// NOT: ESP32-C6'da soc/rtc_cntl_reg.h ve soc/rtc.h yok (farklı mimari)
// Brownout kontrolü için esp_private/esp_brownout.h kullanılıyor

#include "config_store.h"
#include "scheduler.h"
#include "network_manager.h"
#include "mail_functions.h"
#include "web_handlers.h"
#include "ota_manager.h"

// Debug: 0=kapalı, 1=kritik, 2=detaylı
#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL 1
#endif

// Kritik loglar her zaman görünür (hata, uyarı, sistem durumu)
#define LOG_CRITICAL(...) Serial.printf(__VA_ARGS__)
#define LOG_INFO(...) if (DEBUG_LEVEL >= 1) Serial.printf(__VA_ARGS__)
#define LOG_DEBUG(...) if (DEBUG_LEVEL >= 2) Serial.printf(__VA_ARGS__)

// FIRMWARE_VERSION artık config_store.h'da tanımlı (tek noktada yönetim)

// Pin tanımları - XIAO ESP32C6
// D10 = GPIO18 (BUTTON), D8 = GPIO20 (RELAY)

constexpr uint8_t BUTTON_PIN = 18;   // D10 -> GPIO18
constexpr uint8_t RELAY_PIN = 20;    // D8 -> GPIO20
constexpr uint32_t BUTTON_DEBOUNCE_MS = 200;
constexpr uint32_t STATUS_PERSIST_INTERVAL_MS = 60000;
constexpr uint32_t PERIODIC_RESTART_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL; // 24 saat

// ⚠️ GÜVENLİK SABİTLERİ
constexpr uint32_t WATCHDOG_TIMEOUT_SEC = 30;      // Watchdog timeout (30 saniye)
constexpr uint32_t HEAP_CRITICAL_THRESHOLD = 20480; // 20KB - kritik heap seviyesi
constexpr uint32_t HEAP_CHECK_INTERVAL_MS = 10000;  // Heap kontrol aralığı (10 saniye)
constexpr uint32_t WIFI_CHECK_INTERVAL_MS = 30000;  // WiFi durum kontrolü (30 saniye)


ConfigStore configStore;
CountdownScheduler scheduler;
LebenSpurNetworkManager networkManager;
MailAgent mailAgent;
WebServer webServer(80);
WebInterface webUI;
DNSServer dnsServer;
OTAManager otaManager;  // OTA Güncelleme Yöneticisi

bool relayLatched = false;
unsigned long lastButtonChange = 0;
bool lastButtonState = true;
unsigned long lastPersist = 0;
unsigned long finalMailSentTime = 0; // Final mail gönderilme zamanı
bool finalMailSent = false; // Final mail gönderildi mi?
unsigned long bootTime = 0; // Cihaz başlangıç zamanı (periyodik restart için)

String deviceId;
String uniqueChipId; // Benzersiz cihaz ID'si (12 karakter hex)

// NOT: Termal sıcaklık sensörü sistemi KALDIRILDI
// Gereksiz RAM ve kod tasarrufu için kaldırıldı

String generateDeviceId() {
    // Format: "SmartKraft LebensSpur ID:XXXXXXXXXXXX" (benzersiz 12 karakter)
    return "SmartKraft LebensSpur ID:" + uniqueChipId;
}

String generateAPName() {
    // Format: "LS-XXXXXXXXXXXX" (benzersiz 12 karakter)
    return "LS-" + uniqueChipId;
}

void initHardware() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);   // ← PC817C için LOW = Kapalı (LED sönük)
    relayLatched = false;
}

void latchRelay(bool state) {
    relayLatched = state;
    digitalWrite(RELAY_PIN, state ? HIGH : LOW);  // ← PC817C: true=HIGH (LED yanar, röle tetiklenir)
}

void resetTimerFromButton() {
    latchRelay(false);
    scheduler.reset();
    scheduler.start();
    scheduler.persist();
    
    // Reboot flag'ini sıfırla
    finalMailSent = false;
    finalMailSentTime = 0;
}

void processAlarms() {
    uint8_t alarmIndex = 0;

    if (scheduler.alarmDue(alarmIndex)) {
        ScheduleSnapshot snap = scheduler.snapshot();
        
        if (!networkManager.isConnected()) {
            networkManager.ensureConnected(true);
        }
        
        String error;
        if (!mailAgent.sendWarning(alarmIndex, snap, error)) {
            // Mail başarısız, tekrar denenecek
        } else {
            scheduler.acknowledgeAlarm(alarmIndex);
            scheduler.persist();
        }
        
        yield();
    }

    if (scheduler.finalDue()) {
        ScheduleSnapshot snap = scheduler.snapshot();
        latchRelay(true);
        
        if (!networkManager.isConnected()) {
            networkManager.ensureConnected(true);
        }
        
        TimerRuntime runtime = scheduler.runtimeState();
        
        String error;
        if (!mailAgent.sendFinal(snap, runtime, error)) {
            scheduler.updateRuntime(runtime);
        } else {
            scheduler.acknowledgeFinal();
            scheduler.persist();
            
            if (!finalMailSent) {
                finalMailSent = true;
                finalMailSentTime = millis();
            }
        }
        
        yield();
    }
}

void handleButton() {
    bool state = digitalRead(BUTTON_PIN);
    
    if (state != lastButtonState) {
        if (millis() - lastButtonChange > BUTTON_DEBOUNCE_MS) {
            lastButtonChange = millis();
            lastButtonState = state;
            
            if (state == LOW) {
                resetTimerFromButton();
            }
        }
    }
}

// Güç koruma sistemi
#if __has_include("esp_private/esp_brownout.h")
    #include "esp_private/esp_brownout.h"
#elif __has_include("esp_brownout.h")
    #include "esp_brownout.h"
#endif

struct PowerStats {
    float voltageMin = 5.0f;
    float voltageMax = 0.0f;
    float lastVoltage = 0.0f;
    uint32_t brownoutCount = 0;
    uint32_t resetCount = 0;
    unsigned long lastBrownout = 0;
};
PowerStats powerStats;

RTC_NOINIT_ATTR uint32_t brownoutCounter;
RTC_NOINIT_ATTR uint32_t lastBootTime;

void initPowerMonitoring() {
    powerStats.voltageMin = 5.0f;
    powerStats.voltageMax = 5.0f;
    powerStats.lastVoltage = 5.0f;
    powerStats.brownoutCount = brownoutCounter;
}

void checkPowerQuality() {
    powerStats.lastVoltage = 5.0f;
}

void configurePowerProtection() {
    #if CONFIG_IDF_TARGET_ESP32C6
    // Brownout varsayılan seviyede
    #endif
    
    esp_wifi_set_max_tx_power(60);  // 15 dBm - stabilite modu
    
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_BROWNOUT) {
        brownoutCounter++;
        if (brownoutCounter >= 5) {
            esp_wifi_set_max_tx_power(40);  // 10 dBm
        }
    } else if (reason == ESP_RST_POWERON) {
        brownoutCounter = 0;
    }
}

void disableAllSleepModes() {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    disableWiFiPowerSave();
}

// ============================================
// TERMAL SİSTEM KALDIRILDI
// ============================================
// NOT: Termal sıcaklık sensörü, checkThermalProtection ve ilişkili
// fonksiyonlar gereksiz olduğu için kaldırıldı.

// Boot sebebini string olarak döndür
const char* getResetReasonString() {
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_POWERON:   return "Güç açıldı";
        case ESP_RST_EXT:       return "Harici reset";
        case ESP_RST_SW:        return "Yazılımsal reset (ESP.restart)";
        case ESP_RST_PANIC:     return "⚠️ PANIC - Exception/Crash!";
        case ESP_RST_INT_WDT:   return "⚠️ Interrupt Watchdog timeout!";
        case ESP_RST_TASK_WDT:  return "⚠️ Task Watchdog timeout!";
        case ESP_RST_WDT:       return "⚠️ Watchdog timeout!";
        case ESP_RST_DEEPSLEEP: return "Deep sleep'ten çıkış";
        case ESP_RST_BROWNOUT:  return "⚠️ Brownout (düşük voltaj)!";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "?";
    }
}

void setup() {
    Serial.begin(115200);
    delay(100);
    
    // Boot banner - her zaman göster
    Serial.println(F("\n========================================"));
    Serial.println(F("   SmartKraft LebensSpur - Starting..."));
    Serial.println(F("========================================"));
    
    esp_reset_reason_t resetReason = esp_reset_reason();
    LOG_CRITICAL("[BOOT] Reset: %s\n", getResetReasonString());
    LOG_CRITICAL("[BOOT] Free heap: %lu bytes\n", ESP.getFreeHeap());
    
    if (resetReason == ESP_RST_BROWNOUT) {
        LOG_CRITICAL("[BOOT] ⚠️ BROWNOUT! Güç sorunu tespit edildi!\n");
    } else if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_TASK_WDT || 
               resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_WDT) {
        LOG_CRITICAL("[BOOT] ⚠️ CRASH! Önceki boot çöktü!\n");
    }
    
    randomSeed(esp_random());
    disableAllSleepModes();
    initPowerMonitoring();
    // NOT: initTemperatureSensor() KALDIRILDI
    initHardware();
    configStore.begin();
    
    uniqueChipId = getOrCreateDeviceId();
    deviceId = generateDeviceId();
    LOG_INFO("[BOOT] Device ID: %s\n", uniqueChipId.c_str());
    
    scheduler.begin(&configStore);
    networkManager.begin(&configStore);
    mailAgent.begin(&configStore, &networkManager, deviceId);
    otaManager.begin(FIRMWARE_VERSION);  // OTA Manager başlat (webUI'den önce)
    
    String apName = generateAPName();
    webUI.begin(&webServer, &configStore, &scheduler, &mailAgent, &networkManager, deviceId, &dnsServer, apName, &otaManager);
    
    latchRelay(false);
    lastButtonState = digitalRead(BUTTON_PIN);
    lastButtonChange = millis();
    lastPersist = millis();
    bootTime = millis();
    
    webUI.startServer();
    
    disableWiFiPowerSave();
    esp_wifi_set_max_tx_power(84);
    
    // Watchdog 60 saniye
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 60000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
    if (wdt_err != ESP_OK) {
        esp_task_wdt_deinit();
        esp_task_wdt_init(&wdt_config);
    }
    
    wdt_err = esp_task_wdt_add(NULL);
    
    LOG_CRITICAL("[BOOT] ✓ Firmware: %s | Heap: %lu | MaxBlok: %lu\n", 
                 FIRMWARE_VERSION, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    Serial.println(F("========================================\n"));
}

void loop() {
    static unsigned long lastLoop = 0;
    static unsigned long loopCounter = 0;
    static unsigned long lastWiFiCheck = 0;
    static unsigned long lastHeapCheck = 0;
    static unsigned long lastHealthLog = 0;
    static unsigned long lastNetworkActivity = 0;
    static unsigned long lastPowerCheck = 0;
    static unsigned long lastWebServerRestart = 0; // Web server restart timer
    static uint32_t noActivityCounter = 0;
    
    unsigned long now = millis();
    
    esp_task_wdt_reset();
    
    if (now - lastPowerCheck > 5000) {
        checkPowerQuality();
        lastPowerCheck = now;
    }
    
    // ============================================
    // WEB SERVER PERİYODİK RESTART (6 SAAT)
    // Cache/memory leak sorunlarını önlemek için
    // ============================================
    constexpr uint32_t WEB_SERVER_RESTART_INTERVAL = 6UL * 60UL * 60UL * 1000UL; // 6 saat
    if (now - lastWebServerRestart > WEB_SERVER_RESTART_INTERVAL) {
        LOG_CRITICAL("[WEB] 6 saat doldu, web server yeniden başlatılıyor...\n");
        
        // Web server'a restart
        webServer.stop();
        delay(100);
        webUI.startServer();
        webUI.resetHealthCounter(); // Health counter sıfırla
        
        LOG_CRITICAL("[WEB] Web server yeniden başlatıldı. Heap: %lu\n", ESP.getFreeHeap());
        lastWebServerRestart = now;
    }
    
    // AP modu aktifse veya bağlıysak network activity var
    wifi_mode_t wifiMode = WiFi.getMode();
    bool apActive = (wifiMode == WIFI_AP || wifiMode == WIFI_AP_STA);
    
    if (networkManager.isConnected() || apActive) {
        lastNetworkActivity = now;
        noActivityCounter = 0;
    } else {
        noActivityCounter++;
    }
    
    // Sadece STA modunda ve uzun süre bağlantı yoksa restart
    // AP modunda restart yapma - kullanıcı ayar yapıyor olabilir
    if (!apActive && now - lastNetworkActivity > 600000 && noActivityCounter > 1000) {
        LOG_CRITICAL("[SYS] ⚠️ 10dk ağ yok, restart...\n");
        scheduler.persist();
        delay(100);
        ESP.restart();
    }
    
    // ============================================
    // 5 DAKİKADA BİR SİSTEM DURUMU RAPORU
    // ============================================
    if (now - lastHealthLog > 300000) {
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t minFreeHeap = ESP.getMinFreeHeap();
        uint32_t maxAllocHeap = ESP.getMaxAllocHeap();  // En büyük ayrılabilir blok
        bool wifiOk = networkManager.isConnected();
        wifi_mode_t mode = WiFi.getMode();
        bool apActive = (mode == WIFI_AP || mode == WIFI_AP_STA);
        unsigned long uptimeMin = (now - bootTime) / 60000;
        
        LOG_CRITICAL("\n[STATUS] ========= 5dk Rapor =========\n");
        LOG_CRITICAL("[STATUS] Uptime: %lu dk (%lu saat)\n", uptimeMin, uptimeMin / 60);
        LOG_CRITICAL("[STATUS] Heap: %lu | Min: %lu | MaxBlok: %lu\n", freeHeap, minFreeHeap, maxAllocHeap);
        
        // Heap sağlık durumu göster
        if (maxAllocHeap < 10000) {
            LOG_CRITICAL("[STATUS] ⚠️ HEAP FRAGMANTE!\n");
        } else if (freeHeap < 40000) {
            LOG_CRITICAL("[STATUS] ⚠️ Heap düşük\n");
        }
        
        LOG_CRITICAL("[STATUS] WiFi: %s | AP: %s | RSSI: %d\n", 
                     wifiOk ? "OK" : "KOPUK", 
                     apActive ? "Aktif" : "-",
                     wifiOk ? WiFi.RSSI() : 0);
        LOG_CRITICAL("[STATUS] Timer: %s | Kalan: %lu sn\n",
                     scheduler.isActive() ? "Çalışıyor" : (scheduler.isPaused() ? "Duraklatıldı" : "Durdu"),
                     scheduler.remainingSeconds());
        LOG_CRITICAL("[STATUS] Loop/5dk: %lu | Brownout: %lu\n", loopCounter, powerStats.brownoutCount);
        LOG_CRITICAL("[STATUS] =====================================\n\n");
        
        if (!wifiOk && !apActive) {
            LOG_CRITICAL("[STATUS] WiFi/AP yok, bağlanılıyor...\n");
            esp_task_wdt_reset();
            networkManager.ensureConnected(false);
        }
        
        // Kritik heap uyarısı
        if (freeHeap < 30000) {
            LOG_CRITICAL("[STATUS] ⚠️ Heap düşük! %lu bytes\n", freeHeap);
        }
        
        lastHealthLog = now;
        loopCounter = 0;
    }
    
    webUI.loop();
    yield();
    esp_task_wdt_reset();
    
    scheduler.tick();
    handleButton();
    
    if (now - lastLoop > 500) {
        esp_task_wdt_reset();
        processAlarms();
        lastLoop = now;
        yield();
    }

    if (finalMailSent && (now - finalMailSentTime >= 60000)) {
        scheduler.persist();
        delay(100);
        ESP.restart();
    }

    if (now - lastPersist > STATUS_PERSIST_INTERVAL_MS) {
        scheduler.persist();
        lastPersist = now;
    }
    
    esp_task_wdt_reset();
    mailAgent.processQueue();
    esp_task_wdt_reset();
    
    if (now - lastHeapCheck > HEAP_CHECK_INTERVAL_MS) {
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t minFreeHeap = ESP.getMinFreeHeap();
        uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
        
        // Kritik heap seviyesi
        if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
            LOG_CRITICAL("[SYS] ⚠️ HEAP KRİTİK! %lu < %lu, restart!\n", freeHeap, HEAP_CRITICAL_THRESHOLD);
            scheduler.persist();
            delay(100);
            ESP.restart();
        }
        
        // Heap fragmantasyonu tespiti: Toplam free yeterli ama max blok küçük
        // Bu durumda malloc başarısız olabilir
        if (freeHeap > 40000 && maxAllocHeap < 8000) {
            LOG_CRITICAL("[SYS] ⚠️ HEAP FRAGMANTE! Free:%lu MaxBlok:%lu, restart!\n", freeHeap, maxAllocHeap);
            scheduler.persist();
            delay(100);
            ESP.restart();
        }
        
        lastHeapCheck = now;
    }
    
    // WiFi reconnect - AP modunda değilse ve termal blok yoksa
    static unsigned long lastWiFiReconnect = 0;
    static uint8_t reconnectFailCount = 0;
    
    if (now - lastWiFiReconnect > 30000) {
        if (!networkManager.isConnected()) {
            wifi_mode_t mode = WiFi.getMode();
            // Sadece AP modunda değilse reconnect dene
            if (mode != WIFI_AP) {
                esp_task_wdt_reset();
                bool connected = networkManager.ensureConnected(false);
                
                if (!connected) {
                    reconnectFailCount++;
                    // 5 başarısız denemeden sonra interval'i artır (2dk)
                    if (reconnectFailCount >= 5) {
                        lastWiFiReconnect = now + 90000; // Ekstra 90sn bekle
                        LOG_CRITICAL("[WIFI] 5x başarısız, 2dk bekleniyor\n");
                    }
                } else {
                    reconnectFailCount = 0;
                }
            }
        } else {
            reconnectFailCount = 0;
        }
        lastWiFiReconnect = now;
    }
    
    if (now - lastWiFiCheck > 300000) {
        disableWiFiPowerSave();
        lastWiFiCheck = now;
    }
    
    otaManager.setWiFiConnected(networkManager.isConnected());
    otaManager.loop();
    
    unsigned long uptime = now - bootTime;
    if (now < bootTime) {
        bootTime = now;
        LOG_CRITICAL("[SYS] millis() overflow, bootTime sıfırlandı\n");
    } else if (uptime >= PERIODIC_RESTART_INTERVAL_MS) {
        LOG_CRITICAL("[SYS] 24 saat doldu, planlı restart\n");
        scheduler.persist();
        delay(100);
        ESP.restart();
    }
    
    yield();
    loopCounter++;
    
    // Çok kısa gecikme yerine yield yeterli - CPU'yu meşgul tutmadan
    // delayMicroseconds(100) kaldırıldı - gereksiz ve CPU israfı
}
