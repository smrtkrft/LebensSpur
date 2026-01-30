#pragma once

#include <WebServer.h>
#include <DNSServer.h>
#include "scheduler.h"
#include "mail_functions.h"
#include "network_manager.h"
#include "config_store.h"
#include "ota_manager.h"

// Performans optimizasyonları için tanımlar
#define JSON_CAPACITY_SMALL 512    // Küçük JSON responses için
#define JSON_CAPACITY_MEDIUM 1024  // Orta JSON responses için
#define JSON_CAPACITY_LARGE 2048   // Büyük JSON responses için
#define SERIAL_DEBUG_MINIMAL       // Verbose serial debug'ı azalt

class WebInterface {
public:
    void begin(WebServer *server,
               ConfigStore *store,
               CountdownScheduler *scheduler,
               MailAgent *mail,
               LebenSpurNetworkManager *netManager,
               const String &deviceId,
               DNSServer *dns = nullptr,
               const String &apName = "SmartKraft-LebensSpur",
               OTAManager *ota = nullptr);

    void startServer();
    void loop();

    void broadcastStatus();
    
    // ===== WEB SERVER HEALTH CHECK =====
    bool isHealthy() const;               // Server sağlıklı mı?
    uint32_t getLastRequestTime() const;  // Son istek zamanı
    void resetHealthCounter();            // Health counter'ı sıfırla

private:
    WebServer *server = nullptr;
    ConfigStore *store = nullptr;
    CountdownScheduler *scheduler = nullptr;
    MailAgent *mail = nullptr;
    LebenSpurNetworkManager *network = nullptr;
    DNSServer *dnsServer = nullptr;
    OTAManager *otaManager = nullptr;  // OTA Manager referansı
    String deviceId;
    String apName;

    unsigned long lastStatusPush = 0;
    unsigned long lastRequestTime = 0;       // Son başarılı istek zamanı
    uint32_t requestCounter = 0;             // Toplam istek sayısı
    static constexpr uint32_t HEALTH_TIMEOUT_MS = 300000; // 5 dakika istek yoksa unhealthy
    
    // Performance optimizations - cache frequently accessed data
    unsigned long lastStatusCache = 0;
    String cachedStatusResponse;
    static const unsigned long STATUS_CACHE_DURATION = 1000; // 1 saniye cache

    void handleIndex();
    void handleStatus();
    void handleTimerGet();
    void handleTimerUpdate();
    void handleTimerStart();
    void handleTimerStop();
    void handleTimerResume();
    void handleTimerReset();
    void handleVirtualButton();

    void handleMailGet();
    void handleMailUpdate();
    void handleMailTest();

    void handleWiFiGet();
    void handleWiFiUpdate();
    void handleWiFiScan();

    void handleAPIGet();      // ⚠️ YENİ: API ayarlarını getir
    void handleAPIUpdate();   // ⚠️ YENİ: API ayarlarını kaydet
    void handleAPITrigger();  // ⚠️ YENİ: Dinamik endpoint handler

    void handleFactoryReset();
    void handleReboot();
    void handleOTACheck();  // OTA güncelleme kontrolü

    void handleAttachmentList();
    void handleAttachmentUpload();
    void handleAttachmentDelete();
    
    void handleI18n();

    void handleLogs();
    void sendJson(const JsonDocument &doc);
    
    // Helper functions
    // getChipIdHex() artık config_store.h'da global
    void startAPModeMDNS(); // Start mDNS for AP mode
};
