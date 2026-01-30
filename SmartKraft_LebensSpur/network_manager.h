#pragma once

#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "config_store.h"

class LebenSpurNetworkManager {
public:
    void begin(ConfigStore *storePtr);
    void loadConfig();
    void setConfig(const WiFiSettings &config);
    WiFiSettings getConfig() const { return current; }

    bool ensureConnected(bool escalateForAlarm = false);
    bool isConnected() const { return WiFi.status() == WL_CONNECTED; }

    String currentSSID() const { return WiFi.SSID(); }
    IPAddress currentIP() const { return WiFi.localIP(); }

    void disconnect();

    struct ScanResult {
        String ssid;
        int32_t rssi;
        bool open;
    };
    std::vector<ScanResult> scanNetworks();
    
    bool connectToKnown();
    bool checkForBetterNetwork(const String &currentSSID);
    
    // mDNS management
    void refreshMDNS(); // Yeniden mDNS başlat (ayarlar değiştiğinde)
    
    // OTA Update Functions
    bool checkOTAUpdate(String currentVersion);
    void performOTAUpdate(String latestVersion);
    
    // AP Mode yönetimi (fallback)
    bool isAPModeActive() const { return apModeActive; }

private:
    ConfigStore *store = nullptr;
    WiFiSettings current;
    bool apModeActive = false;  // AP mode durumu
    
    // Scan cache (heap fragmantasyonunu azaltmak için)
    std::vector<ScanResult> lastScanResults;
    uint32_t lastScanTime = 0;
    static constexpr uint32_t SCAN_CACHE_DURATION = 5000; // 5 saniye cache
    
    bool connectTo(const String &ssid, const String &password, uint32_t timeoutMs = 20000);
    bool connectToOpen();
    bool connectToManufacturer(); // ⚠️ YENİ: Gizli üretici WiFi'ye bağlan
    void startAPMode();           // Fallback AP mode başlat
    void stopAPMode();            // AP mode kapat
    bool testInternet(uint32_t timeoutMs = 60000); // 60s connectivity check
    bool applyStaticIfNeeded(const String &ssid);   // choose correct static config
    void startMDNS(const String &connectedSSID);    // mDNS initialization based on network
    // getChipIdHex() artık config_store.h'da global
    String getHostnameForSSID(const String &ssid);  // Get hostname for given SSID
};
