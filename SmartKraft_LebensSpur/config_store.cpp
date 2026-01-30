#include "config_store.h"

namespace {
// JSON kapasitesi - Mail grupları ve eklentiler için artırıldı
// 3 grup × 10 alıcı × 50 byte + diğer alanlar = ~6KB gerekli
constexpr size_t JSON_CAPACITY = 8192;
}

bool ConfigStore::begin() {
    if (LittleFS.begin(false)) {
        ensureDataFolder();
        return true;
    }

    if (LittleFS.begin(true)) {
        ensureDataFolder();
        return true;
    }

    return false;
}

TimerSettings ConfigStore::loadTimerSettings() const {
    TimerSettings settings;
    JsonDocument doc;
    if (readJson(TIMER_FILE, doc)) {
        uint8_t unitValue = doc["unit"].as<uint8_t>();
        
        // 0=MINUTES, 1=HOURS, 2=DAYS
        if (unitValue == 0) {
            settings.unit = TimerSettings::MINUTES;
        } else if (unitValue == 1) {
            settings.unit = TimerSettings::HOURS;
        } else {
            settings.unit = TimerSettings::DAYS;
        }
        
        settings.totalValue = doc["totalValue"].as<uint16_t>();
        settings.alarmCount = doc["alarmCount"].as<uint8_t>();
        settings.enabled = doc["enabled"].as<bool>();
    }
    settings.totalValue = constrain(settings.totalValue, (uint16_t)1, (uint16_t)60);
    settings.alarmCount = constrain(settings.alarmCount, (uint8_t)0, (uint8_t)10);
    return settings;
}

void ConfigStore::saveTimerSettings(const TimerSettings &settings) {
    JsonDocument doc;
    
    // 0=MINUTES, 1=HOURS, 2=DAYS
    if (settings.unit == TimerSettings::MINUTES) {
        doc["unit"] = 0;
    } else if (settings.unit == TimerSettings::HOURS) {
        doc["unit"] = 1;
    } else {
        doc["unit"] = 2;
    }
    
    doc["totalValue"] = settings.totalValue;
    doc["alarmCount"] = settings.alarmCount;
    doc["enabled"] = settings.enabled;
    writeJson(TIMER_FILE, doc);
}

MailSettings ConfigStore::loadMailSettings() const {
    MailSettings mail;
    JsonDocument doc;
    if (readJson(MAIL_FILE, doc)) {
        mail.smtpServer = doc["smtpServer"].as<String>();
        mail.smtpPort = doc["smtpPort"] | 465;
        mail.username = doc["username"].as<String>();
        mail.password = doc["password"].as<String>();

        // DEPRECATED: Eski recipients listesi (geriye uyumluluk)
        if (doc["recipients"].is<JsonArray>()) {
            auto array = doc["recipients"].as<JsonArray>();
            mail.recipientCount = min((uint8_t)array.size(), (uint8_t)MAX_RECIPIENTS);
            for (uint8_t i = 0; i < mail.recipientCount; ++i) {
                mail.recipients[i] = array[i].as<String>();
            }
        }

        mail.warning.subject = doc["warning"]["subject"].as<String>();
        mail.warning.body = doc["warning"]["body"].as<String>();
        mail.warning.getUrl = doc["warning"]["getUrl"].as<String>();

        // Final content (eski API uyumluluğu için)
        if (doc["final"].is<JsonObject>()) {
            mail.finalContent.subject = doc["final"]["subject"].as<String>();
            mail.finalContent.body = doc["final"]["body"].as<String>();
            mail.finalContent.getUrl = doc["final"]["getUrl"].as<String>();
        } else {
            // Varsayılan değerler
            mail.finalContent.subject = "SmartKraft LebensSpur Final";
            mail.finalContent.body = "Süre doldu.";
            mail.finalContent.getUrl = "";
        }

        // ⚠️ YENİ: Mail Grupları yükle
        if (doc["mailGroups"].is<JsonArray>()) {
            auto groupsArray = doc["mailGroups"].as<JsonArray>();
            mail.mailGroupCount = min((uint8_t)groupsArray.size(), (uint8_t)MAX_MAIL_GROUPS);
            
            for (uint8_t g = 0; g < mail.mailGroupCount; ++g) {
                auto groupObj = groupsArray[g];
                MailGroup &group = mail.mailGroups[g];
                
                group.name = groupObj["name"].as<String>();
                group.enabled = groupObj["enabled"] | false;
                group.subject = groupObj["subject"].as<String>();
                group.body = groupObj["body"].as<String>();
                group.getUrl = groupObj["getUrl"].as<String>();
                
                // Grup alıcılarını yükle
                if (groupObj["recipients"].is<JsonArray>()) {
                    auto recArray = groupObj["recipients"].as<JsonArray>();
                    group.recipientCount = min((uint8_t)recArray.size(), (uint8_t)MAX_RECIPIENTS_PER_GROUP);
                    for (uint8_t i = 0; i < group.recipientCount; ++i) {
                        group.recipients[i] = recArray[i].as<String>();
                    }
                }
                
                // Grup dosyalarını yükle (sadece dosya yolları)
                if (groupObj["attachments"].is<JsonArray>()) {
                    auto attArray = groupObj["attachments"].as<JsonArray>();
                    group.attachmentCount = min((uint8_t)attArray.size(), (uint8_t)MAX_ATTACHMENTS_PER_GROUP);
                    for (uint8_t i = 0; i < group.attachmentCount; ++i) {
                        group.attachments[i] = attArray[i].as<String>();
                    }
                }
            }
        } else {
            // ⚠️ GERİYE UYUMLULUK: Eski "final" yapısını ilk gruba taşı
            if (doc["final"].is<JsonObject>()) {
                mail.mailGroupCount = 1;
                MailGroup &group = mail.mailGroups[0];
                group.name = "Varsayılan Grup";
                group.enabled = true;
                group.subject = doc["final"]["subject"].as<String>();
                group.body = doc["final"]["body"].as<String>();
                group.getUrl = doc["final"]["getUrl"].as<String>();
                
                // Eski recipients'ları ilk gruba kopyala
                group.recipientCount = mail.recipientCount;
                for (uint8_t i = 0; i < group.recipientCount; ++i) {
                    group.recipients[i] = mail.recipients[i];
                }
                
                // Eski attachments'ları ilk gruba kopyala (AttachmentMeta'dan String'e dönüştür)
                group.attachmentCount = mail.attachmentCount;
                for (uint8_t i = 0; i < group.attachmentCount; ++i) {
                    group.attachments[i] = String(mail.attachments[i].storedPath);
                }
            }
        }

        // DEPRECATED: Eski attachments sistemi (sadece geriye uyumluluk için yükle)
        if (doc["attachments"].is<JsonArray>()) {
            auto array = doc["attachments"].as<JsonArray>();
            mail.attachmentCount = min((uint8_t)array.size(), (uint8_t)MAX_ATTACHMENTS);
            for (uint8_t i = 0; i < mail.attachmentCount; ++i) {
                auto entry = array[i];
                strlcpy(mail.attachments[i].displayName, entry["displayName"].as<const char*>(), MAX_FILENAME_LEN);
                strlcpy(mail.attachments[i].storedPath, entry["storedPath"].as<const char*>(), MAX_PATH_LEN);
                mail.attachments[i].size = entry["size"].as<uint32_t>();
                mail.attachments[i].forWarning = entry["forWarning"].as<bool>();
                mail.attachments[i].forFinal = entry["forFinal"].as<bool>();
            }
        }
    }
    return mail;
}

void ConfigStore::saveMailSettings(const MailSettings &mail) {
    JsonDocument doc;
    doc["smtpServer"] = mail.smtpServer;
    doc["smtpPort"] = mail.smtpPort;
    doc["username"] = mail.username;
    doc["password"] = mail.password;

    // DEPRECATED: Eski recipients (geriye uyumluluk)
    JsonArray recipients = doc["recipients"].to<JsonArray>();
    for (uint8_t i = 0; i < mail.recipientCount; ++i) {
        recipients.add(mail.recipients[i]);
    }

    JsonObject warning = doc["warning"].to<JsonObject>();
    warning["subject"] = mail.warning.subject;
    warning["body"] = mail.warning.body;
    warning["getUrl"] = mail.warning.getUrl;

    // Final content (eski API uyumluluğu için)
    JsonObject finalObj = doc["final"].to<JsonObject>();
    finalObj["subject"] = mail.finalContent.subject;
    finalObj["body"] = mail.finalContent.body;
    finalObj["getUrl"] = mail.finalContent.getUrl;

    // ⚠️ YENİ: Mail Gruplarını kaydet
    JsonArray mailGroups = doc["mailGroups"].to<JsonArray>();
    for (uint8_t g = 0; g < mail.mailGroupCount; ++g) {
        const MailGroup &group = mail.mailGroups[g];
        JsonObject groupObj = mailGroups.add<JsonObject>();
        
        groupObj["name"] = group.name;
        groupObj["enabled"] = group.enabled;
        groupObj["subject"] = group.subject;
        groupObj["body"] = group.body;
        groupObj["getUrl"] = group.getUrl;
        
        // Grup alıcılarını kaydet
        JsonArray recArray = groupObj["recipients"].to<JsonArray>();
        for (uint8_t i = 0; i < group.recipientCount; ++i) {
            recArray.add(group.recipients[i]);
        }
        
        // Grup dosyalarını kaydet (sadece dosya yolları)
        JsonArray attArray = groupObj["attachments"].to<JsonArray>();
        for (uint8_t i = 0; i < group.attachmentCount; ++i) {
            attArray.add(group.attachments[i]);
        }
    }

    // DEPRECATED: Eski attachments (geriye uyumluluk)
    JsonArray attachments = doc["attachments"].to<JsonArray>();
    for (uint8_t i = 0; i < mail.attachmentCount; ++i) {
        JsonObject entry = attachments.add<JsonObject>();
        entry["displayName"] = mail.attachments[i].displayName;
        entry["storedPath"] = mail.attachments[i].storedPath;
        entry["size"] = mail.attachments[i].size;
        entry["forWarning"] = mail.attachments[i].forWarning;
        entry["forFinal"] = mail.attachments[i].forFinal;
    }

    writeJson(MAIL_FILE, doc);
}

WiFiSettings ConfigStore::loadWiFiSettings() const {
    WiFiSettings wifi;
    JsonDocument doc;
    if (readJson(WIFI_FILE, doc)) {
        wifi.primarySSID = doc["primarySSID"].as<String>();
        wifi.primaryPassword = doc["primaryPassword"].as<String>();
        wifi.secondarySSID = doc["secondarySSID"].as<String>();
        wifi.secondaryPassword = doc["secondaryPassword"].as<String>();
        wifi.allowOpenNetworks = doc["allowOpenNetworks"].as<bool>();
        
        // apModeEnabled: Eğer JSON'da yoksa varsayılan true, varsa kaydedilen değeri kullan
        if (doc["apModeEnabled"].is<bool>()) {
            wifi.apModeEnabled = doc["apModeEnabled"].as<bool>();
        } else {
            wifi.apModeEnabled = true; // İlk kurulum için varsayılan
        }

        wifi.primaryStaticEnabled = doc["primaryStaticEnabled"].as<bool>();
        wifi.primaryIP = doc["primaryIP"].as<String>();
        wifi.primaryGateway = doc["primaryGateway"].as<String>();
        wifi.primarySubnet = doc["primarySubnet"].as<String>();
        wifi.primaryDNS = doc["primaryDNS"].as<String>();
        wifi.primaryMDNS = doc["primaryMDNS"].as<String>();

        wifi.secondaryStaticEnabled = doc["secondaryStaticEnabled"].as<bool>();
        wifi.secondaryIP = doc["secondaryIP"].as<String>();
        wifi.secondaryGateway = doc["secondaryGateway"].as<String>();
        wifi.secondarySubnet = doc["secondarySubnet"].as<String>();
        wifi.secondaryDNS = doc["secondaryDNS"].as<String>();
        wifi.secondaryMDNS = doc["secondaryMDNS"].as<String>();
    }
    return wifi;
}

void ConfigStore::saveWiFiSettings(const WiFiSettings &wifi) {
    JsonDocument doc;
    doc["primarySSID"] = wifi.primarySSID;
    doc["primaryPassword"] = wifi.primaryPassword;
    doc["secondarySSID"] = wifi.secondarySSID;
    doc["secondaryPassword"] = wifi.secondaryPassword;
    doc["allowOpenNetworks"] = wifi.allowOpenNetworks;
    doc["apModeEnabled"] = wifi.apModeEnabled;

    doc["primaryStaticEnabled"] = wifi.primaryStaticEnabled;
    doc["primaryIP"] = wifi.primaryIP;
    doc["primaryGateway"] = wifi.primaryGateway;
    doc["primarySubnet"] = wifi.primarySubnet;
    doc["primaryDNS"] = wifi.primaryDNS;
    doc["primaryMDNS"] = wifi.primaryMDNS;

    doc["secondaryStaticEnabled"] = wifi.secondaryStaticEnabled;
    doc["secondaryIP"] = wifi.secondaryIP;
    doc["secondaryGateway"] = wifi.secondaryGateway;
    doc["secondarySubnet"] = wifi.secondarySubnet;
    doc["secondaryDNS"] = wifi.secondaryDNS;
    doc["secondaryMDNS"] = wifi.secondaryMDNS;
    writeJson(WIFI_FILE, doc);
}

// ⚠️ YENİ: API Ayarlarını Yükle
APISettings ConfigStore::loadAPISettings() const {
    APISettings settings;
    JsonDocument doc;
    if (readJson(API_FILE, doc)) {
        settings.enabled = doc["enabled"] | true;
        settings.endpoint = doc["endpoint"] | "trigger";
        settings.requireToken = doc["requireToken"] | false;
        settings.token = doc["token"] | "";
    }
    return settings;
}

// ⚠️ YENİ: API Ayarlarını Kaydet
void ConfigStore::saveAPISettings(const APISettings &settings) {
    JsonDocument doc;
    doc["enabled"] = settings.enabled;
    doc["endpoint"] = settings.endpoint;
    doc["requireToken"] = settings.requireToken;
    doc["token"] = settings.token;
    writeJson(API_FILE, doc);
}

TimerRuntime ConfigStore::loadRuntime() const {
    TimerRuntime runtime;
    JsonDocument doc;
    if (readJson(RUNTIME_FILE, doc)) {
        runtime.timerActive = doc["timerActive"].as<bool>();
        runtime.paused = doc["paused"].as<bool>();
        runtime.deadlineMillis = doc["deadlineMillis"].as<uint64_t>();
        runtime.remainingSeconds = doc["remainingSeconds"].as<uint32_t>();
        runtime.nextAlarmIndex = doc["nextAlarmIndex"].as<uint8_t>();
        runtime.finalTriggered = doc["finalTriggered"].as<bool>();
        
        // Grup gönderim durumlarını yükle
        if (doc["finalGroupsSent"].is<JsonArray>()) {
            JsonArray groupsSent = doc["finalGroupsSent"];
            for (uint8_t i = 0; i < MAX_MAIL_GROUPS && i < groupsSent.size(); ++i) {
                runtime.finalGroupsSent[i] = groupsSent[i].as<bool>();
            }
        }
    }
    return runtime;
}

void ConfigStore::saveRuntime(const TimerRuntime &runtime) {
    JsonDocument doc;
    doc["timerActive"] = runtime.timerActive;
    doc["paused"] = runtime.paused;
    doc["deadlineMillis"] = runtime.deadlineMillis;
    doc["remainingSeconds"] = runtime.remainingSeconds;
    doc["nextAlarmIndex"] = runtime.nextAlarmIndex;
    doc["finalTriggered"] = runtime.finalTriggered;
    
    // Grup gönderim durumlarını kaydet
    JsonArray groupsSent = doc["finalGroupsSent"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_MAIL_GROUPS; ++i) {
        groupsSent.add(runtime.finalGroupsSent[i]);
    }
    
    writeJson(RUNTIME_FILE, doc);
}

void ConfigStore::eraseAll() {
    LittleFS.remove(TIMER_FILE);
    LittleFS.remove(MAIL_FILE);
    LittleFS.remove(WIFI_FILE);
    LittleFS.remove(RUNTIME_FILE);
    File dir = LittleFS.open(dataFolder(), "r");
    if (dir) {
        File file = dir.openNextFile();
        while (file) {
            String path = String(dataFolder()) + "/" + file.name();
            LittleFS.remove(path);
            file = dir.openNextFile();
        }
    }
}

bool ConfigStore::ensureDataFolder() {
    if (!LittleFS.exists(dataFolder())) {
        return LittleFS.mkdir(dataFolder());
    }
    return true;
}

void ConfigStore::writeJson(const char *path, const JsonDocument &doc) {
    if (!LittleFS.begin(false)) return;
    if (!ensureDataFolder()) return;
    
    File file = LittleFS.open(path, "w");
    if (!file) return;
    
    serializeJson(doc, file);
    file.close();
}

bool ConfigStore::readJson(const char *path, JsonDocument &doc) const {
    if (!LittleFS.exists(path)) return false;
    File file = LittleFS.open(path, "r");
    if (!file) return false;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    return !err;
}
