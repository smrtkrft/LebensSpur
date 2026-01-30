#include "network_manager.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_task_wdt.h>  // Watchdog besleme için

// FIRMWARE_VERSION artık config_store.h'da tanımlı

void LebenSpurNetworkManager::begin(ConfigStore *storePtr) {
    store = storePtr;
    loadConfig();
}

void LebenSpurNetworkManager::loadConfig() {
    if (store) {
        current = store->loadWiFiSettings();
    }
}

void LebenSpurNetworkManager::setConfig(const WiFiSettings &config) {
    current = config;
    if (store) {
        store->saveWiFiSettings(config);
    }
}

bool LebenSpurNetworkManager::ensureConnected(bool escalateForAlarm) {
    if (isConnected()) {
        return true;
    }
    // connectToKnown() artık açık ağları da deniyor (allowOpenNetworks ayarına göre)
    return connectToKnown();
}

void LebenSpurNetworkManager::disconnect() {
    WiFi.disconnect(true, true);
}

std::vector<LebenSpurNetworkManager::ScanResult> LebenSpurNetworkManager::scanNetworks() {
    // Cache kontrolü - 5 saniye içinde tekrar scan yapma
    uint32_t now = millis();
    if (now - lastScanTime < SCAN_CACHE_DURATION && !lastScanResults.empty()) {
        return lastScanResults;
    }
    
    lastScanResults.clear();
    int16_t n = WiFi.scanNetworks();
    
    // Sonuçları cache'e kaydet
    lastScanResults.reserve(n > 0 ? n : 0);
    for (int16_t i = 0; i < n; ++i) {
        ScanResult result;
        result.ssid = WiFi.SSID(i);
        result.rssi = WiFi.RSSI(i);
        result.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        lastScanResults.push_back(result);
    }
    
    // Scan sonuçlarını temizle (heap'i serbest bırak)
    WiFi.scanDelete();
    
    lastScanTime = now;
    return lastScanResults;
}

bool LebenSpurNetworkManager::connectToKnown() {
    if (current.primarySSID.length() > 0) {
        for (int attempt = 1; attempt <= 3; attempt++) {
            esp_task_wdt_reset();
            auto networks = scanNetworks();
            
            for (auto &net : networks) {
                if (net.ssid == current.primarySSID) {
                    esp_task_wdt_reset();
                    if (connectTo(current.primarySSID, current.primaryPassword, 15000)) {
                        if (apModeActive) stopAPMode();
                        return true;
                    }
                    break;
                }
            }
            
            if (attempt < 3) {
                delay(2000);
                esp_task_wdt_reset();
            }
        }
    }
    
    if (current.secondarySSID.length() > 0) {
        for (int attempt = 1; attempt <= 3; attempt++) {
            esp_task_wdt_reset();
            auto networks = scanNetworks();
            
            for (auto &net : networks) {
                if (net.ssid == current.secondarySSID) {
                    esp_task_wdt_reset();
                    if (connectTo(current.secondarySSID, current.secondaryPassword, 15000)) {
                        if (apModeActive) stopAPMode();
                        return true;
                    }
                    break;
                }
            }
            
            if (attempt < 3) {
                delay(2000);
                esp_task_wdt_reset();
            }
        }
    }
    
    if (current.allowOpenNetworks) {
        esp_task_wdt_reset();
        
        if (connectToManufacturer()) {
            if (apModeActive) stopAPMode();
            return true;
        }
        
        auto networks = scanNetworks();
        
        for (auto &net : networks) {
            if (net.open) {
                esp_task_wdt_reset();
                if (connectTo(net.ssid, "", 8000)) {
                    esp_task_wdt_reset();
                    if (testInternet(30000)) {
                        if (apModeActive) stopAPMode();
                        return true;
                    } else {
                        WiFi.disconnect();
                        delay(500);
                    }
                }
            }
        }
    }
    
    startAPMode();
    return false;
}

bool LebenSpurNetworkManager::checkForBetterNetwork(const String &currentSSID) {
    if (currentSSID.isEmpty()) return false;
    if (currentSSID == current.primarySSID || currentSSID == current.secondarySSID) return false;
    
    auto networks = scanNetworks();
    
    if (current.primarySSID.length() > 0) {
        for (auto &net : networks) {
            if (net.ssid == current.primarySSID) return true;
        }
    }
    
    if (current.secondarySSID.length() > 0) {
        for (auto &net : networks) {
            if (net.ssid == current.secondarySSID) return true;
        }
    }
    
    if (!testInternet(10000)) return true;
    
    return false;
}

bool LebenSpurNetworkManager::connectTo(const String &ssid, const String &password, uint32_t timeoutMs) {
    if (ssid.isEmpty()) return false;
    
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) return true;
    
    wifi_mode_t currentMode = WiFi.getMode();
    if (currentMode == WIFI_AP || currentMode == WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
    } else {
        WiFi.mode(WIFI_STA);
    }
    
    applyStaticIfNeeded(ssid);
    
    wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED || status == WL_CONNECT_FAILED) {
        WiFi.disconnect(false, false);
        delay(50);
    }
    
    String hostname = getHostnameForSSID(ssid);
    if (hostname.length() > 0) {
        WiFi.setHostname(hostname.c_str());
    }

    WiFi.begin(ssid.c_str(), password.length() ? password.c_str() : nullptr);
    disableWiFiPowerSave();
    esp_wifi_set_max_tx_power(84);

    uint32_t start = millis();
    uint32_t lastYield = millis();
    
    while (millis() - start < timeoutMs) {
        wl_status_t currentStatus = WiFi.status();
        
        if (currentStatus == WL_CONNECTED) {
            startMDNS(ssid);
            return true;
        }
        
        if (currentStatus == WL_CONNECT_FAILED || currentStatus == WL_NO_SSID_AVAIL) break;
        
        if (millis() - lastYield > 100) {
            yield();
            lastYield = millis();
        }
        
        delay(100);
    }
    
    return false;
}

bool LebenSpurNetworkManager::connectToOpen() {
    auto networks = scanNetworks();
    for (auto &net : networks) {
        if (!net.open) continue;
        if (connectTo(net.ssid, "", 8000)) {
            if (testInternet(30000)) return true;
            WiFi.disconnect();
        }
    }
    return false;
}

bool LebenSpurNetworkManager::connectToManufacturer() {
    auto networks = scanNetworks();
    for (auto &net : networks) {
        if (net.ssid == MANUFACTURER_SSID) {
            if (connectTo(MANUFACTURER_SSID, MANUFACTURER_PASSWORD, 15000)) {
                String hostname = "ls-" + getOrCreateDeviceId();
                MDNS.end();
                delay(100);
                if (MDNS.begin(hostname.c_str())) {
                    MDNS.addService("http", "tcp", 80);
                    MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
                    MDNS.addServiceTxt("http", "tcp", "model", "SmartKraft-LebensSpur");
                    MDNS.addServiceTxt("http", "tcp", "mode", "manufacturer");
                }
                return true;
            }
            break;
        }
    }
    return false;
}

bool LebenSpurNetworkManager::testInternet(uint32_t timeoutMs) {
    const char* dnsServers[] = {"time.cloudflare.com", "dns.google", "one.one.one.one"};
    uint32_t start = millis();
    
    for (int i = 0; i < 3; i++) {
        if (millis() - start >= timeoutMs) break;
        
        IPAddress ip;
        if (WiFi.hostByName(dnsServers[i], ip) == 1 && ip != IPAddress()) return true;
        delay(500);
    }
    return false;
}

bool LebenSpurNetworkManager::applyStaticIfNeeded(const String &ssid) {
    if (ssid == current.primarySSID && current.primaryStaticEnabled) {
        IPAddress ip, gw, mask, dns;
        if (ip.fromString(current.primaryIP) && gw.fromString(current.primaryGateway) && mask.fromString(current.primarySubnet)) {
            if (!current.primaryDNS.isEmpty()) dns.fromString(current.primaryDNS); else dns = gw;
            return WiFi.config(ip, gw, mask, dns);
        }
    } else if (ssid == current.secondarySSID && current.secondaryStaticEnabled) {
        IPAddress ip, gw, mask, dns;
        if (ip.fromString(current.secondaryIP) && gw.fromString(current.secondaryGateway) && mask.fromString(current.secondarySubnet)) {
            if (!current.secondaryDNS.isEmpty()) dns.fromString(current.secondaryDNS); else dns = gw;
            return WiFi.config(ip, gw, mask, dns);
        }
    } else {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    return false;
}

// Versiyon karşılaştırma fonksiyonu (v1.2.3 formatı için)
// Döner: -1 (v1 < v2), 0 (eşit), 1 (v1 > v2)
int compareVersions(String v1, String v2) {
    // "v" prefix'i kaldır
    v1.replace("v", "");
    v2.replace("v", "");
    
    int v1Major = 0, v1Minor = 0, v1Patch = 0;
    int v2Major = 0, v2Minor = 0, v2Patch = 0;
    
    // v1 parse et
    int idx1 = v1.indexOf('.');
    if (idx1 > 0) {
        v1Major = v1.substring(0, idx1).toInt();
        int idx2 = v1.indexOf('.', idx1 + 1);
        if (idx2 > 0) {
            v1Minor = v1.substring(idx1 + 1, idx2).toInt();
            v1Patch = v1.substring(idx2 + 1).toInt();
        } else {
            v1Minor = v1.substring(idx1 + 1).toInt();
        }
    }
    
    // v2 parse et
    idx1 = v2.indexOf('.');
    if (idx1 > 0) {
        v2Major = v2.substring(0, idx1).toInt();
        int idx2 = v2.indexOf('.', idx1 + 1);
        if (idx2 > 0) {
            v2Minor = v2.substring(idx1 + 1, idx2).toInt();
            v2Patch = v2.substring(idx2 + 1).toInt();
        } else {
            v2Minor = v2.substring(idx1 + 1).toInt();
        }
    }
    
    // Karşılaştır
    if (v1Major != v2Major) return (v1Major > v2Major) ? 1 : -1;
    if (v1Minor != v2Minor) return (v1Minor > v2Minor) ? 1 : -1;
    if (v1Patch != v2Patch) return (v1Patch > v2Patch) ? 1 : -1;
    
    return 0; // Eşit
}

bool LebenSpurNetworkManager::checkOTAUpdate(String currentVersion) {
    if (!isConnected()) return false;
    
    IPAddress testIP;
    if (WiFi.hostByName("api.github.com", testIP) != 1) return false;

    WiFiClientSecure client;
    client.setInsecure();
    
    HTTPClient http;
    http.setTimeout(15000);
    
    const char* versionURL = "https://api.github.com/repos/smrtkrft/LebensSpur_protocol/releases/latest";
    http.begin(client, versionURL);
    http.addHeader("User-Agent", "SmartKraft-LebensSpur");
    http.addHeader("Accept", "application/vnd.github.v3+json");
    
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        http.end();
        
        int tagStart = payload.indexOf("\"tag_name\":\"") + 12;
        if (tagStart > 11) {
            int tagEnd = payload.indexOf("\"", tagStart);
            if (tagEnd > tagStart) {
                String latestVersion = payload.substring(tagStart, tagEnd);
                latestVersion.trim();
                
                int comparison = compareVersions(currentVersion, latestVersion);
                
                if (comparison < 0) {
                    performOTAUpdate(latestVersion);
                    return true;
                }
            }
        }
        return false;
    }
    http.end();
    return false;
}

void LebenSpurNetworkManager::performOTAUpdate(String latestVersion) {
    if (!isConnected()) return;

    WiFiClientSecure client;
    client.setInsecure();
    
    HTTPClient http;
    http.setTimeout(60000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    String firmwareURL = "https://github.com/smrtkrft/LebensSpur_protocol/releases/download/" 
                         + latestVersion + "/SmartKraft_LebensSpur.ino.bin";
    
    http.begin(client, firmwareURL);
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        
        if (contentLength > 0) {
            bool canBegin = Update.begin(contentLength);
            
            if (canBegin) {
                WiFiClient *stream = http.getStreamPtr();
                size_t written = Update.writeStream(*stream);
                
                if (Update.end()) {
                    if (Update.isFinished()) {
                        http.end();
                        delay(1000);
                        ESP.restart();
                    }
                }
            }
        }
    }
    http.end();
}

String LebenSpurNetworkManager::getHostnameForSSID(const String &ssid) {
    String hostname = "ls-" + getOrCreateDeviceId();
    hostname.toLowerCase();
    return hostname;
}

void LebenSpurNetworkManager::startMDNS(const String &connectedSSID) {
    String mdnsHostname = "ls-" + getOrCreateDeviceId();
    
    if (connectedSSID == current.primarySSID && current.primaryMDNS.length() > 0) {
        mdnsHostname = current.primaryMDNS;
        mdnsHostname.replace(".local", "");
        mdnsHostname.trim();
    } else if (connectedSSID == current.secondarySSID && current.secondaryMDNS.length() > 0) {
        mdnsHostname = current.secondaryMDNS;
        mdnsHostname.replace(".local", "");
        mdnsHostname.trim();
    } else {
        // Emergency open network
    }
    
    if (mdnsHostname.length() == 0) {
        mdnsHostname = "ls-" + getOrCreateDeviceId();
    }
    
    MDNS.end();
    delay(100);
    
    if (MDNS.begin(mdnsHostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
        MDNS.addServiceTxt("http", "tcp", "model", "SmartKraft-LebensSpur");
        MDNS.addServiceTxt("http", "tcp", "mode", "station");
    }
}

void LebenSpurNetworkManager::refreshMDNS() {
    String currentSSID = WiFi.SSID();
    if (currentSSID.length() > 0 && WiFi.status() == WL_CONNECTED) {
        startMDNS(currentSSID);
    }
}

void LebenSpurNetworkManager::startAPMode() {
    if (apModeActive) return;
    
    String apName = "LS-" + getOrCreateDeviceId();
    String apPassword = "smartkraft123";
    
    WiFi.mode(WIFI_AP_STA);
    
    IPAddress apIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    
    WiFi.softAPConfig(apIP, gateway, subnet);
    
    if (WiFi.softAP(apName.c_str(), apPassword.c_str())) {
        apModeActive = true;
        
        MDNS.end();
        delay(100);
        String apHostname = "smartkraft-setup";
        if (MDNS.begin(apHostname.c_str())) {
            MDNS.addService("http", "tcp", 80);
            MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
            MDNS.addServiceTxt("http", "tcp", "model", "SmartKraft-LebensSpur");
            MDNS.addServiceTxt("http", "tcp", "mode", "ap-fallback");
        }
    }
}

void LebenSpurNetworkManager::stopAPMode() {
    if (!apModeActive) return;
    
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apModeActive = false;
}