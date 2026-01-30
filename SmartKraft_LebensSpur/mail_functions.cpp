#include "mail_functions.h"

#include <LittleFS.h>
#include <base64.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <algorithm>  // std::sort için

// ============================================================================
// ROOT CA CERTIFICATES (SSL/TLS Sertifika Doğrulama)
// ============================================================================

// ISRG Root X1 - Let's Encrypt (ProtonMail kullanıyor)
// Geçerlilik: 2015-06-04 → 2035-06-04 (20 YIL!)
const char* ROOT_CA_ISRG_X1 = 
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n";

// ============================================================================

void MailAgent::begin(ConfigStore *storePtr, LebenSpurNetworkManager *netMgrPtr, const String &deviceIdStr) {
    store = storePtr;
    netManager = netMgrPtr;
    deviceId = deviceIdStr;
    if (store) {
        settings = store->loadMailSettings();
    }
    
    // Persistent mail queue'yu yükle
    loadQueueFromStorage();
}

void MailAgent::updateConfig(const MailSettings &config) {
    settings = config;
    if (store) {
        store->saveMailSettings(settings);
    }
}

// ============================================================================
// URL VALIDATION - SSRF KORUMASII
// ============================================================================

bool MailAgent::isValidURL(const String &url) {
    if (url.length() == 0) return false;

    String checkIP = "";
    int ipStart = url.indexOf("://");
    if (ipStart >= 0) {
        ipStart += 3;
        int ipEnd = url.indexOf(':', ipStart);
        if (ipEnd < 0) ipEnd = url.indexOf('/', ipStart);
        if (ipEnd < 0) ipEnd = url.length();
        checkIP = url.substring(ipStart, ipEnd);
    } else {
        checkIP = url;
    }

    if (checkIP.startsWith("192.168.11.")) return true;

    if (checkIP.startsWith("192.168.") || 
        checkIP.startsWith("10.") || 
        checkIP.startsWith("172.16.") || checkIP.startsWith("172.17.") ||
        checkIP.startsWith("172.18.") || checkIP.startsWith("172.19.") ||
        checkIP.startsWith("172.20.") || checkIP.startsWith("172.21.") ||
        checkIP.startsWith("172.22.") || checkIP.startsWith("172.23.") ||
        checkIP.startsWith("172.24.") || checkIP.startsWith("172.25.") ||
        checkIP.startsWith("172.26.") || checkIP.startsWith("172.27.") ||
        checkIP.startsWith("172.28.") || checkIP.startsWith("172.29.") ||
        checkIP.startsWith("172.30.") || checkIP.startsWith("172.31.")) {
        return false;
    }

    if (checkIP.equals("127.0.0.1") || checkIP.equals("localhost") || checkIP.equals("::1")) {
        return false;
    }

    return true;
}

// ============================================================================

String MailAgent::base64Encode(const String &input) {
    return base64::encode(input);
}

String MailAgent::smtpReadLine(WiFiClientSecure &client, uint32_t timeoutMs) {
    String line = "";
    unsigned long start = millis();
    
    while (millis() - start < timeoutMs) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') break;
            if (c != '\r') line += c;
        }
        delay(1);
    }
    
    return line;
}

bool MailAgent::smtpConnect(WiFiClientSecure &client, String &errorMessage) {
    if (WiFi.status() != WL_CONNECTED) {
        errorMessage = "WiFi not connected";
        return false;
    }
    
    // ============================================
    // WiFiClientSecure OPTIMIZATION
    // ============================================
    // NOT: setBufferSizes() ESP32 3.3.x'te mevcut değil
    // Varsayılan buffer boyutları kullanılıyor
    
    if (settings.smtpServer.indexOf("protonmail") >= 0 || 
        settings.smtpServer.indexOf("proton.me") >= 0) {
        client.setCACert(ROOT_CA_ISRG_X1);
    } else {
        client.setInsecure();
    }
    client.setTimeout(10); // 15 -> 10 saniye (daha hızlı timeout)
    
    if (settings.smtpPort == 587) {
        errorMessage = "Port 587 not supported. Use port 465";
        return false;
    }
    
    IPAddress serverIP;
    if (!WiFi.hostByName(settings.smtpServer.c_str(), serverIP)) {
        errorMessage = "DNS failed: " + settings.smtpServer;
        return false;
    }
    
    if (!client.connect(settings.smtpServer.c_str(), settings.smtpPort)) {
        errorMessage = "Connection failed";
        return false;
    }
    
    String response = smtpReadLine(client);
    if (!response.startsWith("220")) {
        errorMessage = "Server greeting failed";
        client.stop();
        return false;
    }
    
    return true;
}

bool MailAgent::smtpAuth(WiFiClientSecure &client, String &errorMessage) {
    String ehloCmd = "EHLO " + String(WiFi.getHostname()) + "\r\n";
    client.print(ehloCmd);
    
    bool foundAuth = false;
    for (int i = 0; i < 10; i++) {
        String response = smtpReadLine(client);
        if (response.indexOf("AUTH") >= 0) foundAuth = true;
        if (response.startsWith("250 ")) break;
    }
    
    if (!foundAuth) {
        errorMessage = "SMTP AUTH desteklenmiyor";
        return false;
    }
    
    client.print("AUTH LOGIN\r\n");
    String response = smtpReadLine(client);
    
    if (!response.startsWith("334")) {
        errorMessage = "AUTH LOGIN reddedildi";
        return false;
    }
    
    client.println(base64Encode(settings.username));
    response = smtpReadLine(client);
    
    if (!response.startsWith("334")) {
        errorMessage = "Kullanıcı adı reddedildi";
        return false;
    }
    
    client.println(base64Encode(settings.password));
    response = smtpReadLine(client);
    
    if (!response.startsWith("235")) {
        errorMessage = "Kimlik doğrulama başarısız - Şifre yanlış";
        return false;
    }
    
    return true;
}

bool MailAgent::sendWarning(uint8_t alarmIndex, const ScheduleSnapshot &snapshot, String &errorMessage) {
    String remaining = formatElapsed(snapshot);
    String timestamp = formatHeader();
    
    String subject = settings.warning.subject;
    replaceTemplateVars(subject, deviceId, timestamp, remaining);
    subject.replace("%ALARM_INDEX%", String(alarmIndex + 1));
    subject.replace("%TOTAL_ALARMS%", String(snapshot.totalAlarms));

    String body = settings.warning.body;
    replaceTemplateVars(body, deviceId, timestamp, remaining);
    body.replace("%ALARM_INDEX%", String(alarmIndex + 1));
    body.replace("%TOTAL_ALARMS%", String(snapshot.totalAlarms));

    bool mailSuccess = sendEmailToSelf(subject, body, true, errorMessage);
    
    if (!mailSuccess) {
        enqueueWarning(alarmIndex, snapshot);
        errorMessage = "Mail kuyruğa alındı, arka planda gönderilecek";
    }
    
    if (settings.warning.getUrl.length() > 0 && WiFi.status() == WL_CONNECTED) {
        if (!isValidURL(settings.warning.getUrl)) {
            return mailSuccess;
        }
        
        String taskName = "WarnURL_" + String(alarmIndex);
        xTaskCreate([](void* param) {
            String url = *((String*)param);
            HTTPClient http;
            
            if (url.startsWith("https://")) {
                WiFiClientSecure* client = new WiFiClientSecure();
                client->setInsecure();
                
                if (http.begin(*client, url)) {
                    http.setTimeout(8000);
                    http.setConnectTimeout(3000);
                    http.GET();
                    http.end();
                }
                delete client;
            } else {
                WiFiClient* client = new WiFiClient();
                if (http.begin(*client, url)) {
                    http.setTimeout(8000);
                    http.setConnectTimeout(3000);
                    http.GET();
                    http.end();
                }
                delete client;
            }
            
            delete (String*)param;
            vTaskDelete(NULL);
        }, taskName.c_str(), 8192, new String(settings.warning.getUrl), 1, NULL);
        
        Serial.println(F("[Warning URL] Task başlatıldı (non-blocking)"));
    } else {
        if (settings.warning.getUrl.length() == 0) {
            Serial.println(F("[Warning URL] URL tanımlanmamış, tetiklenmedi"));
        } else {
            Serial.println(F("[Warning URL] ATLANDΙ - WiFi bağlantısı yok"));
        }
    }
    
    return mailSuccess;
}

bool MailAgent::sendFinal(const ScheduleSnapshot &snapshot, TimerRuntime &runtime, String &errorMessage) {
    Serial.println(F("========== LEBENSSPUR PROTOKOLÜ - ÇOKLU GRUP MAİL GÖNDERİMİ =========="));
    
    // Hiç aktif grup yoksa hata
    if (settings.mailGroupCount == 0) {
        errorMessage = "Hiç mail grubu tanımlanmamış";
        Serial.println(F("[Final] HATA: Mail grubu yok"));
        return false;
    }
    
    bool allSuccess = true;
    String lastError = "";
    uint8_t totalMailsSent = 0;
    
    // Her mail grubunu işle
    for (uint8_t g = 0; g < settings.mailGroupCount; ++g) {
        const MailGroup &group = settings.mailGroups[g];
        
        // Grup aktif değilse atla
        if (!group.enabled) {
            Serial.printf("[Final] Grup %d (%s) - ATLANDΙ (devre dışı)\n", g + 1, group.name.c_str());
            runtime.finalGroupsSent[g] = true; // Devre dışı gruplar "gönderildi" sayılır
            continue;
        }
        
        // ✅ ZATEN GÖNDERİLMİŞSE ATLA
        if (runtime.finalGroupsSent[g]) {
            Serial.printf("[Final] Grup %d (%s) - ZATEN GÖNDERİLDİ (atlanıyor)\n", g + 1, group.name.c_str());
            continue;
        }
        
        Serial.printf("\n[Final] ========== GRUP %d: %s ==========\n", g + 1, group.name.c_str());
        Serial.printf("[Final] Alıcı sayısı: %d\n", group.recipientCount);
        Serial.printf("[Final] Dosya sayısı: %d\n", group.attachmentCount);
        
        // Grubun alıcısı yoksa uyar ama devam et
        if (group.recipientCount == 0) {
            Serial.printf("[Final] UYARI: Grup '%s' için alıcı yok\n", group.name.c_str());
            runtime.finalGroupsSent[g] = true; // Alıcısız gruplar "gönderildi" sayılır
            continue;
        }
        
        // Grup subject ve body hazırla
        String subject = group.subject;
        if (subject.startsWith("[TEST LS] ")) {
            subject = subject.substring(10);
            Serial.printf("[Final] [TEST LS] prefix kaldırıldı\n");
        }
        String timestamp = formatHeader();
        replaceTemplateVars(subject, deviceId, timestamp, "0");

        String body = group.body;
        replaceTemplateVars(body, deviceId, timestamp, "0");
        
        // ⚠️ STACK KORUMASI: MailSettings çok büyük, kopyalamıyoruz
        // Sadece attachments pointer'ını geçici değiştirip geri alıyoruz
        uint8_t originalAttachmentCount = settings.attachmentCount;
        AttachmentMeta originalAttachments[MAX_ATTACHMENTS];
        
        // Sadece attachment verilerini kopyala (küçük)
        for (uint8_t i = 0; i < originalAttachmentCount; ++i) {
            originalAttachments[i] = settings.attachments[i];
        }
        
        // Grup attachments'larını geçici olarak settings'e yükle
        // NOT: MailGroup.attachments String[], ama settings.attachments AttachmentMeta[]
        settings.attachmentCount = group.attachmentCount;
        for (uint8_t i = 0; i < group.attachmentCount; ++i) {
            // String dosya yolunu AttachmentMeta'ya dönüştür
            strlcpy(settings.attachments[i].storedPath, group.attachments[i].c_str(), MAX_PATH_LEN);
            settings.attachments[i].displayName[0] = '\0'; // Boş - gönderimde kullanılmayacak
            settings.attachments[i].size = 0; // Bilinmiyor
            settings.attachments[i].forWarning = false;
            settings.attachments[i].forFinal = true;
        }
        
        // Grup alıcılarına AYRI AYRI mail gönder (LebensSpur Protokolü - Privacy)
        bool groupSuccess = true;
        for (uint8_t i = 0; i < group.recipientCount; ++i) {
            if (group.recipients[i].length() == 0) continue;
            
            Serial.printf("[Final] Grup %d - Alıcı %d/%d: %s\n", 
                g + 1, i + 1, group.recipientCount, group.recipients[i].c_str());
            
            String recipientError;
            if (!sendEmailToRecipient(group.recipients[i], subject, body, true, recipientError)) {
                Serial.printf("[Final] ✗ HATA - %s: %s\n", group.recipients[i].c_str(), recipientError.c_str());
                groupSuccess = false;
                allSuccess = false;
                lastError = recipientError;
                break; // Grup başarısız, döngüden çık (bir sonraki denemede devam eder)
            } else {
                Serial.printf("[Final] ✓ BAŞARILI - %s\n", group.recipients[i].c_str());
                totalMailsSent++;
            }
            
            // SMTP sunucuya yük bindirmemek için kısa bekleme
            delay(200);
        }
        
        // ✅ GRUP BAŞARILI İSE İŞARETLE
        if (groupSuccess) {
            runtime.finalGroupsSent[g] = true;
            Serial.printf("[Final] Grup %d (%s) - ✓ TÜM MAİLLER GÖNDERİLDİ\n", g + 1, group.name.c_str());
        } else {
            Serial.printf("[Final] Grup %d (%s) - ✗ HATALI, bir sonraki denemede tekrar gönderilecek\n", g + 1, group.name.c_str());
        }
        
        // Orijinal attachments'ları geri yükle
        settings.attachmentCount = originalAttachmentCount;
        for (uint8_t i = 0; i < originalAttachmentCount; ++i) {
            settings.attachments[i] = originalAttachments[i];
        }
        
        // Grup URL tetiklemesi (sadece grup başarılıysa)
        if (groupSuccess && group.getUrl.length() > 0 && WiFi.status() == WL_CONNECTED) {
            // URL Validation - SSRF Koruması
            if (!isValidURL(group.getUrl)) {
                Serial.printf("[Final URL] Grup %d - ✗ GÜVENLİK: URL reddedildi\n", g + 1);
            } else {
                Serial.printf("[Final URL] Grup %d (%s) - Tetikleniyor: %s\n", 
                    g + 1, group.name.c_str(), group.getUrl.c_str());
                
                // Task oluştur - HEMEN başlat (delay yok)
                String taskName = "FinalURL_G" + String(g);
                xTaskCreate([](void* param) {
                    String url = *((String*)param);
                    
                    HTTPClient http;
                    
                    if (url.startsWith("https://")) {
                        WiFiClientSecure* client = new WiFiClientSecure();
                        client->setInsecure();
                        
                        if (http.begin(*client, url)) {
                            http.setTimeout(8000);
                            http.setConnectTimeout(3000);
                            int httpCode = http.GET();
                            Serial.printf("[Final URL Task] Sonuç: %d\n", httpCode);
                            http.end();
                        }
                        delete client;
                    } else {
                        WiFiClient* client = new WiFiClient();
                        
                        if (http.begin(*client, url)) {
                            http.setTimeout(8000);
                            http.setConnectTimeout(3000);
                            int httpCode = http.GET();
                            Serial.printf("[Final URL Task] Sonuç: %d\n", httpCode);
                            http.end();
                        }
                        delete client;
                    }
                    
                    delete (String*)param;
                    vTaskDelete(NULL);
                }, taskName.c_str(), 8192, new String(group.getUrl), 1, NULL);
                
                Serial.printf("[Final URL] Grup %d - Task başlatıldı (non-blocking)\n", g + 1);
            }
        }
    }
    
    Serial.printf("\n========== LEBENSSPUR PROTOKOLÜ TAMAMLANDI - Toplam %d mail gönderildi ==========\n", totalMailsSent);
    
    // ===== MAIL QUEUE: Başarısız olursa kuyruğa ekle =====
    if (!allSuccess) {
        errorMessage = "Bazı alıcılara mail gönderilemedi: " + lastError;
        Serial.println(F("[Final] Başarısız mailler kuyruğa ekleniyor..."));
        enqueueFinal(snapshot, runtime);
        errorMessage += " - Kuyrukta yeniden denenecek";
    }
    
    return allSuccess;
}

// TEST FONKSIYONLARI - Sadece gönderen adrese mail atar
bool MailAgent::sendWarningTest(const ScheduleSnapshot &snapshot, String &errorMessage) {
    String remaining = formatElapsed(snapshot);
    String timestamp = formatHeader();
    
    String subject = settings.warning.subject;
    replaceTemplateVars(subject, deviceId, timestamp, remaining);
    subject.replace("%ALARM_INDEX%", "1");
    subject.replace("%TOTAL_ALARMS%", String(snapshot.totalAlarms));

    String body = settings.warning.body;
    replaceTemplateVars(body, deviceId, timestamp, remaining);
    body.replace("%ALARM_INDEX%", "1");
    body.replace("%TOTAL_ALARMS%", String(snapshot.totalAlarms));

    // SMTP kullanıcı adına (kendine) gönder - Warning test
    bool mailSuccess = sendEmailToSelf(subject, body, true, errorMessage);
    Serial.printf("[MAIL TEST] Warning mail gönderimi: %s\n", mailSuccess ? "BAŞARILI" : "BAŞARISIZ");
    
    // URL tetikleme (Warning test - NON-BLOCKING)
    if (settings.warning.getUrl.length() > 0 && WiFi.status() == WL_CONNECTED) {
        // URL Validation - SSRF Koruması
        if (!isValidURL(settings.warning.getUrl)) {
            Serial.println(F("[TEST Warning URL] ✗ GÜVENLİK: URL reddedildi (whitelist dışı)"));
            return mailSuccess; // URL tetiklemeden mail sonucunu döndür
        }
        
        Serial.printf("[TEST Warning URL] Tetikleniyor (paralel): %s\n", settings.warning.getUrl.c_str());
        
        // Task oluştur - HEMEN başlat
        xTaskCreate([](void* param) {
            String url = *((String*)param);
            
            // ⚠️ OPTİMİZASYON: Delay kaldırıldı
            
            HTTPClient http;
            
            // HTTP veya HTTPS'e göre client seç
            if (url.startsWith("https://")) {
                WiFiClientSecure* client = new WiFiClientSecure();
                
                // SSL/TLS - Sertifika doğrulaması yok
                client->setInsecure();
                Serial.println(F("[TEST Warning URL] SSL: Insecure mode"));
                
                if (http.begin(*client, url)) {
                    http.setTimeout(8000);
                    http.setConnectTimeout(3000);
                    
                    int httpCode = http.GET();
                    Serial.printf("[TEST Warning URL] Sonuç: %d\n", httpCode);
                    
                    if (httpCode > 0) {
                        String response = http.getString();
                        Serial.printf("[TEST Warning URL] Yanıt: %d bytes\n", response.length());
                        if (response.length() < 150) {
                            Serial.printf("[TEST Warning URL] %s\n", response.c_str());
                        }
                    } else {
                        Serial.printf("[TEST Warning URL] HATA: %s\n", http.errorToString(httpCode).c_str());
                    }
                    http.end();
                }
                delete client;
            } else {
                WiFiClient* client = new WiFiClient();
                
                if (http.begin(*client, url)) {
                    http.setTimeout(8000);
                    http.setConnectTimeout(3000);
                    
                    int httpCode = http.GET();
                    Serial.printf("[TEST Warning URL] Sonuç: %d\n", httpCode);
                    
                    if (httpCode > 0) {
                        String response = http.getString();
                        Serial.printf("[TEST Warning URL] Yanıt: %d bytes\n", response.length());
                        if (response.length() < 150) {
                            Serial.printf("[TEST Warning URL] %s\n", response.c_str());
                        }
                    } else {
                        Serial.printf("[TEST Warning URL] HATA: %s\n", http.errorToString(httpCode).c_str());
                    }
                    http.end();
                }
                delete client;
            }
            
            delete (String*)param;
            vTaskDelete(NULL);
        }, "WarningURLTask", 8192, new String(settings.warning.getUrl), 1, NULL);
        
        Serial.println(F("[TEST Warning URL] Task başlatıldı (non-blocking)"));
    } else {
        if (settings.warning.getUrl.length() == 0) {
            Serial.println(F("[TEST Warning URL] ATLANDΙ - URL boş"));
        } else {
            Serial.println(F("[TEST Warning URL] ATLANDΙ - WiFi bağlantısı yok"));
        }
    }
    
    return mailSuccess;
}

bool MailAgent::sendFinalTest(const ScheduleSnapshot &snapshot, String &errorMessage) {
    Serial.println(F("========== LEBENSSPUR TEST MAİL - İLK AKTİF GRUP =========="));
    
    // İlk aktif grubu bul
    int activeGroupIndex = -1;
    for (uint8_t g = 0; g < settings.mailGroupCount; ++g) {
        if (settings.mailGroups[g].enabled) {
            activeGroupIndex = g;
            break;
        }
    }
    
    if (activeGroupIndex < 0) {
        errorMessage = "Aktif mail grubu bulunamadı";
        Serial.println(F("[Final Test] HATA: Hiç aktif grup yok"));
        return false;
    }
    
    const MailGroup &group = settings.mailGroups[activeGroupIndex];
    Serial.printf("[Final Test] Test edilen grup: %s (Grup %d)\n", group.name.c_str(), activeGroupIndex + 1);
    
    // [TEST LS] otomatik temizleme
    String subject = group.subject;
    if (subject.startsWith("[TEST LS] ")) {
        subject = subject.substring(10);
        Serial.printf("[Final Test] [TEST LS] prefix kaldırıldı\n");
    }
    String timestamp = formatHeader();
    replaceTemplateVars(subject, deviceId, timestamp, "0");

    String body = group.body;
    replaceTemplateVars(body, deviceId, timestamp, "0");

    // Grup alıcıları kontrolü
    if (group.recipientCount == 0) {
        errorMessage = "Bu grubun alıcısı yok";
        return false;
    }

    // STACK-SAFE: Sadece attachment array'ini swap et (büyük struct kopyalama YAPMA!)
    AttachmentMeta originalAttachments[MAX_ATTACHMENTS];
    uint8_t originalAttachmentCount = settings.attachmentCount;
    
    // Orijinal attachments'ı sakla
    for (uint8_t i = 0; i < originalAttachmentCount; ++i) {
        originalAttachments[i] = settings.attachments[i];
    }
    
    // Grup attachments'larını geçici olarak settings'e yükle
    // NOT: MailGroup.attachments String[], ama settings.attachments AttachmentMeta[]
    settings.attachmentCount = group.attachmentCount;
    for (uint8_t i = 0; i < group.attachmentCount; ++i) {
        // String dosya yolunu AttachmentMeta'ya dönüştür
        strlcpy(settings.attachments[i].storedPath, group.attachments[i].c_str(), MAX_PATH_LEN);
        settings.attachments[i].displayName[0] = '\0'; // Boş
        settings.attachments[i].size = 0;
        settings.attachments[i].forWarning = false;
        settings.attachments[i].forFinal = true;
    }

    bool allSuccess = true;
    String lastError = "";
    
    // Her alıcıya AYRI MAIL gönder (LebensSpur protokolü - privacy)
    for (uint8_t i = 0; i < group.recipientCount; ++i) {
        if (group.recipients[i].length() == 0) continue;
        
        Serial.printf("[Final Test] Alıcı %d/%d: %s\n", i + 1, group.recipientCount, group.recipients[i].c_str());
        
        String recipientError;
        if (!sendEmailToRecipient(group.recipients[i], subject, body, true, recipientError)) {
            Serial.printf("[Final Test] ✗ HATA - %s: %s\n", group.recipients[i].c_str(), recipientError.c_str());
            allSuccess = false;
            lastError = recipientError;
        } else {
            Serial.printf("[Final Test] ✓ BAŞARILI - %s\n", group.recipients[i].c_str());
        }
        
        delay(200);
    }
    
    // Orijinal attachments'ları geri yükle
    settings.attachmentCount = originalAttachmentCount;
    for (uint8_t i = 0; i < originalAttachmentCount; ++i) {
        settings.attachments[i] = originalAttachments[i];
    }
    
    if (!allSuccess) {
        errorMessage = "Bazı alıcılara test maili gönderilemedi: " + lastError;
    }
    
    bool mailSuccess = allSuccess;
    Serial.printf("[MAIL TEST] Final/LebensSpur test mail sonucu: %s\n", mailSuccess ? "BAŞARILI" : "BAŞARISIZ");
    
    // URL tetikleme (grup URL'si - NON-BLOCKING)
    if (group.getUrl.length() > 0 && WiFi.status() == WL_CONNECTED) {
        if (!isValidURL(group.getUrl)) {
            Serial.println(F("[TEST Final URL] ✗ GÜVENLİK: URL reddedildi"));
            return mailSuccess;
        }
        
        Serial.printf("[TEST Final URL] Tetikleniyor: %s\n", group.getUrl.c_str());
        
        xTaskCreate([](void* param) {
            String url = *((String*)param);
            HTTPClient http;
            
            if (url.startsWith("https://")) {
                WiFiClientSecure* client = new WiFiClientSecure();
                client->setInsecure();
                
                if (http.begin(*client, url)) {
                    http.setTimeout(8000);
                    http.setConnectTimeout(3000);
                    int httpCode = http.GET();
                    Serial.printf("[TEST Final URL] Sonuç: %d\n", httpCode);
                    http.end();
                }
                delete client;
            } else {
                WiFiClient* client = new WiFiClient();
                
                if (http.begin(*client, url)) {
                    http.setTimeout(8000);
                    http.setConnectTimeout(3000);
                    int httpCode = http.GET();
                    Serial.printf("[TEST Final URL] Sonuç: %d\n", httpCode);
                    http.end();
                }
                delete client;
            }
            
            delete (String*)param;
            vTaskDelete(NULL);
        }, "TestURLTask", 8192, new String(group.getUrl), 1, NULL);
        
        Serial.println(F("[TEST Final URL] Task başlatıldı"));
    }
    
    return mailSuccess;
}

bool MailAgent::smtpSendMail(WiFiClientSecure &client, const String &subject, const String &body, bool includeAttachments, String &errorMessage) {
    // MAIL FROM
    String mailFrom = "MAIL FROM:<" + settings.username + ">\r\n";
    client.print(mailFrom);
    String response = smtpReadLine(client);
    Serial.printf("[SMTP] << %s\n", response.c_str());
    
    if (!response.startsWith("250")) {
        errorMessage = "MAIL FROM reddedildi";
        return false;
    }
    
    // RCPT TO (her alıcı için)
    for (uint8_t i = 0; i < settings.recipientCount; ++i) {
        if (settings.recipients[i].length() == 0) continue;
        
        String rcptTo = "RCPT TO:<" + settings.recipients[i] + ">\r\n";
        client.print(rcptTo);
        response = smtpReadLine(client);
        Serial.printf("[SMTP] << %s\n", response.c_str());
        
        if (!response.startsWith("250")) {
            errorMessage = "Alıcı reddedildi: " + settings.recipients[i];
            return false;
        }
    }
    
    // DATA
    client.print("DATA\r\n");
    response = smtpReadLine(client);
    Serial.printf("[SMTP] << %s\n", response.c_str());
    
    if (!response.startsWith("354")) {
        errorMessage = "DATA komutu reddedildi";
        return false;
    }
    
    // ⚠️ YENİ: STREAMING MIME MESSAGE (RAM tasarrufu için)
    // String yerine direkt WiFiClientSecure'a yazıyoruz
    String boundary = "----=_SKLS_" + String(random(100000, 999999));
    
    // MIME Headers
    client.print("From: " + settings.username + "\r\n");
    client.print("To: ");
    for (uint8_t i = 0; i < settings.recipientCount; ++i) {
        if (i > 0) client.print(", ");
        client.print(settings.recipients[i]);
    }
    client.print("\r\n");
    client.print("Subject: " + subject + "\r\n");
    client.print("MIME-Version: 1.0\r\n");
    client.print("Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n");
    client.print("\r\n");
    
    // Body
    client.print("--" + boundary + "\r\n");
    client.print("Content-Type: text/plain; charset=UTF-8\r\n");
    client.print("Content-Transfer-Encoding: 8bit\r\n\r\n");
    client.print(body);
    client.print("\r\n");
    
    // Attachments (streaming - RAM efficient)
    if (includeAttachments && settings.attachmentCount > 0) {
        Serial.printf("[SMTP Stream] %d attachment kontrol ediliyor (includeAttachments=%d)\n", settings.attachmentCount, includeAttachments);
        uint8_t addedCount = 0;
        
        for (uint8_t i = 0; i < settings.attachmentCount; ++i) {
            const auto &meta = settings.attachments[i];
            
            // ⚠️ DÜZELTİLDİ: forFinal dosyaları ekle (sendEmail fonksiyonu genelde Final test için kullanılıyor)
            // Eğer dosya forFinal=true ise ekle
            if (!meta.forFinal) {
                Serial.printf("[SMTP Stream] Attachment %d ATLANDI (forFinal=false)\n", i);
                continue;
            }
            
            // Dosya varlık kontrolü
            if (!LittleFS.exists(meta.storedPath)) {
                Serial.printf("[SMTP Stream] Attachment %d ATLANDI (dosya yok: %s)\n", i, meta.storedPath);
                continue;
            }
            
            // Dosya boyutu kontrolü (500KB limit)
            File testFile = LittleFS.open(meta.storedPath, "r");
            if (!testFile) {
                Serial.printf("[SMTP Stream] Attachment %d ATLANDI (açılamadı)\n", i);
                continue;
            }
            size_t fileSize = testFile.size();
            testFile.close();
            
            if (fileSize > 512000) { // 500KB limit
                Serial.printf("[SMTP Stream] Attachment %d ATLANDI (çok büyük: %d bytes > 500KB)\n", i, fileSize);
                continue;
            }
            
            // Stream et (RAM'de biriktirmeden direkt gönder)
            smtpStreamAttachment(client, boundary, meta);
            addedCount++;
        }
        
        Serial.printf("[SMTP Stream] TOPLAM: %d/%d attachment gönderildi\n", addedCount, settings.attachmentCount);
    } else {
        if (!includeAttachments) {
            Serial.println(F("[SMTP Stream] Attachment ekleme kapalı (includeAttachments=false)"));
        } else if (settings.attachmentCount == 0) {
            Serial.println(F("[SMTP Stream] Hiç attachment tanımlanmamış"));
        }
    }
    
    // MIME sonlandırma
    client.print("--" + boundary + "--\r\n");
    client.print("\r\n.\r\n");
    
    response = smtpReadLine(client);
    Serial.printf("[SMTP] << %s\n", response.c_str());
    
    if (!response.startsWith("250")) {
        errorMessage = "Mail gönderimi başarısız";
        return false;
    }
    
    Serial.println(F("[SMTP] Mail başarıyla gönderildi (streaming)"));
    return true;
}

bool MailAgent::smtpCommand(WiFiClientSecure &client, const String &command, const String &expectCode, String &errorMessage) {
    if (command.length() > 0) {
        client.print(command);
    }
    
    String response = smtpReadLine(client);
    Serial.printf("[SMTP] << %s\n", response.c_str());
    
    if (!response.startsWith(expectCode)) {
        errorMessage = "Beklenmeyen yanıt: " + response;
        return false;
    }
    
    return true;
}

bool MailAgent::sendEmail(const String &subject, const String &body, bool includeWarningAttachments, String &errorMessage) {
    if (settings.recipientCount == 0) {
        errorMessage = "Mail listesi boş";
        return false;
    }

    if (settings.smtpServer.length() == 0 || settings.username.length() == 0) {
        errorMessage = "SMTP ayarları eksik";
        return false;
    }

    if (!netManager->ensureConnected(true)) {
        errorMessage = "İnternet bağlantısı yok";
        return false;
    }

    WiFiClientSecure client;

    if (!smtpConnect(client, errorMessage)) {
        client.flush();
        client.stop();
        yield();
        return false;
    }

    if (!smtpAuth(client, errorMessage)) {
        client.flush();
        client.stop();
        yield();
        return false;
    }

    if (!smtpSendMail(client, subject, body, includeWarningAttachments, errorMessage)) {
        client.flush();
        client.stop();
        yield();
        return false;
    }

    // QUIT
    client.print("QUIT\r\n");
    String response = smtpReadLine(client);
    Serial.printf("[SMTP] << %s\n", response.c_str());
    
    client.flush();
    client.stop();
    yield();  // Memory cleanup için zaman tanı
    Serial.println(F("[SMTP] Bağlantı kapatıldı"));
    
    return true;
}

// Test için - sadece gönderen adrese mail atar
bool MailAgent::sendEmailToSelf(const String &subject, const String &body, bool includeWarningAttachments, String &errorMessage) {
    if (settings.smtpServer.length() == 0 || settings.username.length() == 0) {
        errorMessage = "SMTP ayarları eksik";
        return false;
    }

    if (!netManager->ensureConnected(true)) {
        errorMessage = "İnternet bağlantısı yok";
        return false;
    }

    WiFiClientSecure client;

    if (!smtpConnect(client, errorMessage)) {
        client.flush();
        client.stop();
        yield();
        return false;
    }

    if (!smtpAuth(client, errorMessage)) {
        client.flush();
        client.stop();
        yield();
        return false;
    }

    // MAIL FROM
    String mailFrom = "MAIL FROM:<" + settings.username + ">\r\n";
    client.print(mailFrom);
    String response = smtpReadLine(client);
    Serial.printf("[SMTP TEST] << %s\n", response.c_str());
    
    if (!response.startsWith("250")) {
        errorMessage = "MAIL FROM reddedildi";
        client.flush();
        client.stop();
        yield();
        return false;
    }
    
    // RCPT TO - sadece kendine gönder
    String rcptTo = "RCPT TO:<" + settings.username + ">\r\n";
    client.print(rcptTo);
    response = smtpReadLine(client);
    Serial.printf("[SMTP TEST] << %s\n", response.c_str());
    
    if (!response.startsWith("250")) {
        errorMessage = "Alıcı reddedildi";
        client.flush();
        client.stop();
        yield();
        return false;
    }
    
    // DATA
    client.print("DATA\r\n");
    response = smtpReadLine(client);
    Serial.printf("[SMTP TEST] << %s\n", response.c_str());
    
    if (!response.startsWith("354")) {
        errorMessage = "DATA komutu reddedildi";
        client.flush();
        client.stop();
        yield();
        return false;
    }
    
    // ⚠️ YENİ: STREAMING MIME (RAM tasarrufu için)
    String boundary = "----=_SKLS_TEST_" + String(random(100000, 999999));
    
    // MIME Headers (streaming)
    client.print("From: " + settings.username + "\r\n");
    client.print("To: " + settings.username + "\r\n");
    client.print("Subject: " + subject + "\r\n");
    client.print("MIME-Version: 1.0\r\n");
    client.print("Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n");
    client.print("\r\n");
    
    // Body
    client.print("--" + boundary + "\r\n");
    client.print("Content-Type: text/plain; charset=UTF-8\r\n");
    client.print("Content-Transfer-Encoding: 8bit\r\n\r\n");
    client.print(body);
    client.print("\r\n");
    
    // Attachments (streaming - warning için)
    if (includeWarningAttachments && settings.attachmentCount > 0) {
        Serial.printf("[Test Self] %d attachment kontrol ediliyor\n", settings.attachmentCount);
        uint8_t addedCount = 0;
        
        for (uint8_t i = 0; i < settings.attachmentCount; ++i) {
            const auto &meta = settings.attachments[i];
            
            if (!meta.forWarning) {
                Serial.printf("[Test Self] Attachment %d ATLANDI (forWarning=false)\n", i);
                continue;
            }
            
            if (!LittleFS.exists(meta.storedPath)) {
                Serial.printf("[Test Self] Attachment %d ATLANDI (dosya yok: %s)\n", i, meta.storedPath);
                continue;
            }
            
            File testFile = LittleFS.open(meta.storedPath, "r");
            if (!testFile) {
                Serial.printf("[Test Self] Attachment %d ATLANDI (açılamadı)\n", i);
                continue;
            }
            size_t fileSize = testFile.size();
            testFile.close();
            
            if (fileSize > 512000) { // 500KB
                Serial.printf("[Test Self] Attachment %d ATLANDI (çok büyük: %d bytes)\n", i, fileSize);
                continue;
            }
            
            smtpStreamAttachment(client, boundary, meta);
            addedCount++;
        }
        
        Serial.printf("[Test Self] TOPLAM: %d/%d attachment gönderildi\n", addedCount, settings.attachmentCount);
    }
    
    // MIME sonlandırma
    client.print("--" + boundary + "--\r\n");
    client.print("\r\n.\r\n");
    
    response = smtpReadLine(client);
    Serial.printf("[SMTP TEST] << %s\n", response.c_str());
    
    if (!response.startsWith("250")) {
        errorMessage = "Mail gönderimi başarısız";
        client.flush();
        client.stop();
        yield();
        return false;
    }
    
    Serial.println(F("[SMTP TEST] Test maili kendi adresinize gönderildi (streaming)"));
    
    // QUIT
    client.print("QUIT\r\n");
    response = smtpReadLine(client);
    Serial.printf("[SMTP TEST] << %s\n", response.c_str());
    
    client.flush();
    client.stop();
    yield();  // Memory cleanup için zaman tanı
    Serial.println(F("[SMTP TEST] Bağlantı kapatıldı"));
    
    return true;
}

// LebensSpur Protokolü için - Tek alıcıya mail gönder (privacy)
bool MailAgent::sendEmailToRecipient(const String &recipient, const String &subject, const String &body, bool includeWarningAttachments, String &errorMessage) {
    if (settings.smtpServer.length() == 0 || settings.username.length() == 0) {
        errorMessage = "SMTP ayarları eksik";
        return false;
    }

    if (!netManager->ensureConnected(true)) {
        errorMessage = "İnternet bağlantısı yok";
        return false;
    }

    WiFiClientSecure client;

    if (!smtpConnect(client, errorMessage)) {
        client.flush();
        client.stop();
        yield();
        return false;
    }

    if (!smtpAuth(client, errorMessage)) {
        client.flush();
        client.stop();
        yield();
        return false;
    }

    // MAIL FROM
    String mailFrom = "MAIL FROM:<" + settings.username + ">\r\n";
    client.print(mailFrom);
    String response = smtpReadLine(client);
    
    if (!response.startsWith("250")) {
        errorMessage = "MAIL FROM reddedildi";
        client.flush();
        client.stop();
        yield();
        return false;
    }
    
    // RCPT TO - sadece bu alıcıya gönder (privacy)
    String rcptTo = "RCPT TO:<" + recipient + ">\r\n";
    client.print(rcptTo);
    response = smtpReadLine(client);
    
    if (!response.startsWith("250")) {
        errorMessage = "Alıcı reddedildi: " + recipient;
        client.flush();
        client.stop();
        yield();
        return false;
    }
    
    // DATA
    client.print("DATA\r\n");
    response = smtpReadLine(client);
    
    if (!response.startsWith("354")) {
        errorMessage = "DATA komutu reddedildi";
        client.flush();
        client.stop();
        yield();
        return false;
    }
    
    // ⚠️ YENİ: STREAMING MIME (tek alıcı için - privacy)
    String boundary = "----=_SKLS_" + String(random(100000, 999999));
    
    // MIME Headers (streaming)
    client.print("From: " + settings.username + "\r\n");
    client.print("To: " + recipient + "\r\n");  // ← Sadece bu alıcı (privacy)
    client.print("Subject: " + subject + "\r\n");
    client.print("MIME-Version: 1.0\r\n");
    client.print("Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n");
    client.print("\r\n");
    
    // Body
    client.print("--" + boundary + "\r\n");
    client.print("Content-Type: text/plain; charset=UTF-8\r\n");
    client.print("Content-Transfer-Encoding: 8bit\r\n\r\n");
    client.print(body);
    client.print("\r\n");
    
    // Attachments (streaming)
    Serial.printf("[Final Recipient] Attachment streaming - includeWarningAttachments=%d, alıcı=%s\n", includeWarningAttachments, recipient.c_str());
    if (includeWarningAttachments && settings.attachmentCount > 0) {
        uint8_t addedCount = 0;
        
        for (uint8_t i = 0; i < settings.attachmentCount; ++i) {
            const auto &meta = settings.attachments[i];
            
            if (!meta.forFinal) {
                Serial.printf("[Final Recipient] Attachment %d ATLANDI (forFinal=false)\n", i);
                continue;
            }
            
            if (!LittleFS.exists(meta.storedPath)) {
                Serial.printf("[Final Recipient] Attachment %d ATLANDI (dosya yok)\n", i);
                continue;
            }
            
            File testFile = LittleFS.open(meta.storedPath, "r");
            if (!testFile) {
                Serial.printf("[Final Recipient] Attachment %d ATLANDI (açılamadı)\n", i);
                continue;
            }
            size_t fileSize = testFile.size();
            testFile.close();
            
            if (fileSize > 512000) { // 500KB
                Serial.printf("[Final Recipient] Attachment %d ATLANDI (çok büyük: %d bytes)\n", i, fileSize);
                continue;
            }
            
            smtpStreamAttachment(client, boundary, meta);
            addedCount++;
        }
        
        Serial.printf("[Final Recipient] TOPLAM: %d/%d attachment gönderildi\n", addedCount, settings.attachmentCount);
    }
    
    // MIME sonlandırma
    client.print("--" + boundary + "--\r\n");
    client.print("\r\n.\r\n");
    
    response = smtpReadLine(client);
    
    if (!response.startsWith("250")) {
        errorMessage = "Mail gönderimi başarısız: " + recipient;
        client.flush();
        client.stop();
        yield();
        return false;
    }
    
    // QUIT
    client.print("QUIT\r\n");
    smtpReadLine(client);
    
    client.flush();
    client.stop();
    yield();  // Memory cleanup için zaman tanı
    
    return true;
}

String MailAgent::buildMimeMessage(const String &subject, const String &body, bool includeWarningAttachments) {
    // ⚠️ DEPRECATED - Bu fonksiyon artık KULLANILMIYOR
    // Tüm mail fonksiyonları streaming kullanıyor (RAM tasarrufu için)
    Serial.println(F("[DEPRECATED] buildMimeMessage() çağrıldı - lütfen streaming kullanın"));
    return "";
}

// ⚠️ ÖNEMLİ: STREAM ATTACHMENT - RAM TASARRUFU İÇİN
// String yerine direkt WiFiClientSecure'a yazıyoruz
void MailAgent::smtpStreamAttachment(WiFiClientSecure &client, const String &boundary, const AttachmentMeta &meta) {
    Serial.printf("[Stream] Dosya stream ediliyor: %s (%d bytes)\n", meta.displayName, meta.size);
    
    File file = LittleFS.open(meta.storedPath, "r");
    if (!file) {
        Serial.printf("[Stream] HATA: Dosya açılamadı: %s\n", meta.storedPath);
        return;
    }
    
    // MIME type belirleme
    String mimeType = "application/octet-stream";
    
    // MIME headers
    client.print("--" + boundary + "\r\n");
    client.print("Content-Type: " + mimeType + "; name=\"" + String(meta.displayName) + "\"\r\n");
    client.print("Content-Transfer-Encoding: base64\r\n");
    client.print("Content-Disposition: attachment; filename=\"" + String(meta.displayName) + "\"\r\n\r\n");
    
    // Base64 streaming (57-byte chunks = 76-char base64 lines - RFC 2045 compliant)
    const size_t CHUNK_SIZE = 57; // 57*4/3 = 76 chars
    uint8_t buffer[CHUNK_SIZE];
    size_t totalRead = 0;
    size_t fileSize = file.size();
    
    while (file.available() && totalRead < fileSize) {
        size_t bytesRead = file.read(buffer, CHUNK_SIZE);
        
        if (bytesRead > 0) {
            String encoded = base64::encode(buffer, bytesRead);
            client.print(encoded);
            client.print("\r\n");
            totalRead += bytesRead;
            
            // Progress indicator
            if (fileSize > 50000 && (totalRead % 10000) == 0) {
                Serial.printf("[Stream] Gönderildi: %d/%d bytes (%.1f%%)\n", 
                    totalRead, fileSize, (totalRead * 100.0) / fileSize);
            }
            
            // Watchdog reset
            yield();
        } else {
            break;
        }
    }
    
    file.close();
    client.print("\r\n");
    Serial.printf("[Stream] ✓ Dosya tamamen gönderildi: %d bytes\n", totalRead);
}

void MailAgent::appendAttachments(String &mime, const String &boundary, bool warning) {
    // ⚠️ DEPRECATED - Bu fonksiyon artık KULLANILMIYOR
    // smtpStreamAttachment() kullanılıyor (RAM tasarrufu için)
    Serial.println(F("[DEPRECATED] appendAttachments() çağrıldı - lütfen smtpStreamAttachment() kullanın"));
}

String MailAgent::formatHeader() const {
    // millis() tabanlı çalışma süresi (NTP kullanılmıyor)
    unsigned long seconds = millis() / 1000;
    unsigned long days = seconds / 86400;
    seconds %= 86400;
    unsigned long hours = seconds / 3600;
    seconds %= 3600;
    unsigned long minutes = seconds / 60;
    seconds %= 60;
    
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Uptime: %lug %02luh %02lum %02lus", 
             days, hours, minutes, seconds);
    return String(buffer);
}

String MailAgent::formatElapsed(const ScheduleSnapshot &snapshot) const {
    uint32_t rem = snapshot.remainingSeconds;
    uint32_t days = rem / 86400UL;
    rem %= 86400UL;
    uint32_t hours = rem / 3600UL;
    rem %= 3600UL;
    uint32_t minutes = rem / 60UL;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%ud %uh %um", days, hours, minutes);
    return String(buffer);
}

// ============================================================================
// MAIL QUEUE IMPLEMENTATION
// Persistent queue that survives restarts, never expires
// Priority: Warning > Final
// Retry: 5x60s → 10x300s → skip → 600s infinite
// ============================================================================

void MailAgent::loadQueueFromStorage() {
    Serial.println(F("[MailQueue] Kuyruk yükleniyor..."));
    
    if (!LittleFS.exists(QUEUE_FILE)) {
        Serial.println(F("[MailQueue] Kuyruk dosyası yok, boş başlatılıyor"));
        return;
    }
    
    File file = LittleFS.open(QUEUE_FILE, "r");
    if (!file) {
        Serial.println(F("[MailQueue] Kuyruk dosyası açılamadı"));
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("[MailQueue] JSON parse hatası: %s\n", error.c_str());
        return;
    }
    
    mailQueue.clear();
    nextMailId = doc["nextId"] | 1;
    
    JsonArray arr = doc["queue"].as<JsonArray>();
    for (JsonObject obj : arr) {
        QueuedMail mail;
        mail.id = obj["id"] | 0;
        mail.type = static_cast<MailType>(obj["type"].as<uint8_t>());
        mail.phase = static_cast<RetryPhase>(obj["phase"].as<uint8_t>());
        mail.attemptCount = obj["attempts"] | 0;
        mail.nextRetryTime = millis(); // Restart sonrası hemen dene
        mail.createdAt = obj["created"] | 0;
        mail.subject = obj["subject"].as<String>();
        mail.body = obj["body"].as<String>();
        mail.alarmIndex = obj["alarm"] | 0;
        mail.includeAttachments = obj["attach"] | false;
        mail.startTime = obj["startTime"].as<String>();
        mail.endTime = obj["endTime"].as<String>();
        mail.description = obj["desc"].as<String>();
        
        mailQueue.push_back(mail);
    }
    
    sortQueueByPriority();
    Serial.printf("[MailQueue] ✓ %d mail yüklendi\n", mailQueue.size());
}

void MailAgent::saveQueueToStorage() {
    JsonDocument doc;
    doc["nextId"] = nextMailId;
    
    JsonArray arr = doc["queue"].to<JsonArray>();
    for (const auto &mail : mailQueue) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = mail.id;
        obj["type"] = static_cast<uint8_t>(mail.type);
        obj["phase"] = static_cast<uint8_t>(mail.phase);
        obj["attempts"] = mail.attemptCount;
        obj["created"] = mail.createdAt;
        obj["subject"] = mail.subject;
        obj["body"] = mail.body;
        obj["alarm"] = mail.alarmIndex;
        obj["attach"] = mail.includeAttachments;
        obj["startTime"] = mail.startTime;
        obj["endTime"] = mail.endTime;
        obj["desc"] = mail.description;
    }
    
    File file = LittleFS.open(QUEUE_FILE, "w");
    if (!file) {
        Serial.println(F("[MailQueue] Kuyruk kaydedilemedi!"));
        return;
    }
    
    serializeJson(doc, file);
    file.close();
    Serial.printf("[MailQueue] ✓ %d mail kaydedildi\n", mailQueue.size());
}

void MailAgent::clearQueue() {
    mailQueue.clear();
    LittleFS.remove(QUEUE_FILE);
    Serial.println(F("[MailQueue] Kuyruk temizlendi"));
}

bool MailAgent::hasQueuedMails() const {
    return !mailQueue.empty();
}

size_t MailAgent::getQueueSize() const {
    return mailQueue.size();
}

void MailAgent::sortQueueByPriority() {
    // Warning (0) > Final (1)
    // Aynı tip içinde eski olanlar önce (FIFO)
    std::sort(mailQueue.begin(), mailQueue.end(), [](const QueuedMail &a, const QueuedMail &b) {
        if (static_cast<uint8_t>(a.type) != static_cast<uint8_t>(b.type)) {
            return static_cast<uint8_t>(a.type) < static_cast<uint8_t>(b.type);
        }
        return a.createdAt < b.createdAt;
    });
}

uint32_t MailAgent::getRetryInterval(RetryPhase phase) const {
    switch (phase) {
        case RetryPhase::PHASE1:
            return 60 * 1000;     // 60 saniye
        case RetryPhase::PHASE2:
            return 300 * 1000;    // 5 dakika
        case RetryPhase::SKIPPED:
            return 600 * 1000;    // 10 dakika (infinite)
        default:
            return 60 * 1000;
    }
}

void MailAgent::advanceRetryPhase(QueuedMail &mail) {
    mail.attemptCount++;
    
    switch (mail.phase) {
        case RetryPhase::PHASE1:
            if (mail.attemptCount >= 5) {
                Serial.printf("[MailQueue] Mail #%d: PHASE1 tamamlandı (5 deneme), PHASE2'ye geçiliyor\n", mail.id);
                mail.phase = RetryPhase::PHASE2;
                mail.attemptCount = 0;
            }
            break;
            
        case RetryPhase::PHASE2:
            if (mail.attemptCount >= 10) {
                Serial.printf("[MailQueue] Mail #%d: PHASE2 tamamlandı (10 deneme), SKIPPED olarak işaretleniyor\n", mail.id);
                mail.phase = RetryPhase::SKIPPED;
                mail.attemptCount = 0;
            }
            break;
            
        case RetryPhase::SKIPPED:
            // Infinite retry - sayaç artmaya devam eder ama asla silinmez
            Serial.printf("[MailQueue] Mail #%d: SKIPPED aşamasında, deneme #%d (infinite)\n", mail.id, mail.attemptCount);
            break;
    }
    
    mail.nextRetryTime = millis() + getRetryInterval(mail.phase);
}

void MailAgent::enqueueWarning(uint8_t alarmIndex, const ScheduleSnapshot &snapshot) {
    QueuedMail mail;
    mail.id = nextMailId++;
    mail.type = MailType::WARNING;
    mail.phase = RetryPhase::PHASE1;
    mail.attemptCount = 0;
    mail.nextRetryTime = millis(); // Hemen dene
    mail.createdAt = millis();
    mail.alarmIndex = alarmIndex;
    mail.includeAttachments = true;
    
    // Snapshot'tan mevcut bilgileri kaydet
    // Not: startTimeStr, endTimeStr, description snapshot'ta yok
    // Bu değerler gönderim anında settings'ten alınacak
    mail.startTime = "";
    mail.endTime = "";
    mail.description = "Alarm " + String(alarmIndex + 1);
    
    // Subject ve body daha sonra oluşturulacak
    mail.subject = ""; // Gönderim anında oluşturulur
    mail.body = "";
    
    mailQueue.push_back(mail);
    sortQueueByPriority();
    saveQueueToStorage();
    
    Serial.printf("[MailQueue] ✓ Warning mail #%d kuyruğa eklendi (alarm %d)\n", mail.id, alarmIndex);
}

void MailAgent::enqueueFinal(const ScheduleSnapshot &snapshot, TimerRuntime &runtime) {
    QueuedMail mail;
    mail.id = nextMailId++;
    mail.type = MailType::FINAL;
    mail.phase = RetryPhase::PHASE1;
    mail.attemptCount = 0;
    mail.nextRetryTime = millis(); // Hemen dene
    mail.createdAt = millis();
    mail.alarmIndex = 0; // Final için kullanılmaz
    mail.includeAttachments = false; // Final maillerde attachment yok
    
    // Snapshot'tan mevcut bilgileri kaydet
    // Not: startTimeStr, endTimeStr, description snapshot'ta yok
    mail.startTime = "";
    mail.endTime = "";
    mail.description = "Süreç Tamamlandı";
    
    mail.subject = "";
    mail.body = "";
    
    mailQueue.push_back(mail);
    sortQueueByPriority();
    saveQueueToStorage();
    
    Serial.printf("[MailQueue] ✓ Final mail #%d kuyruğa eklendi\n", mail.id);
}

bool MailAgent::trySendQueuedMail(QueuedMail &mail, String &errorMessage) {
    Serial.printf("[MailQueue] Mail #%d gönderiliyor (tip: %s, aşama: %d, deneme: %d)\n",
                  mail.id,
                  mail.type == MailType::WARNING ? "WARNING" : "FINAL",
                  static_cast<int>(mail.phase),
                  mail.attemptCount + 1);
    
    // WiFi kontrolü
    if (!netManager || !netManager->isConnected()) {
        errorMessage = "WiFi bağlı değil";
        return false;
    }
    
    // Mail içeriğini oluştur
    String subject, body;
    
    if (mail.type == MailType::WARNING) {
        // Warning mail subject/body
        subject = "⚠️ [LebensSpur Uyarı] Alarm " + String(mail.alarmIndex + 1) + " - " + mail.description;
        body = "SmartKraft LebensSpur Uyarı Maili\n\n";
        body += "Cihaz ID: " + deviceId + "\n";
        body += "Alarm: " + String(mail.alarmIndex + 1) + "\n";
        body += "Başlangıç: " + mail.startTime + "\n";
        body += "Bitiş: " + mail.endTime + "\n";
        body += "Açıklama: " + mail.description + "\n";
        body += "\n" + formatHeader();
    } else {
        // Final mail subject/body
        subject = "✅ [LebensSpur Final] Süreç Tamamlandı - " + mail.description;
        body = "SmartKraft LebensSpur Final Maili\n\n";
        body += "Cihaz ID: " + deviceId + "\n";
        body += "Başlangıç: " + mail.startTime + "\n";
        body += "Bitiş: " + mail.endTime + "\n";
        body += "Açıklama: " + mail.description + "\n";
        body += "\n" + formatHeader();
    }
    
    // Mail gönder
    bool success = sendEmail(subject, body, mail.includeAttachments, errorMessage);
    
    if (success) {
        Serial.printf("[MailQueue] ✓ Mail #%d başarıyla gönderildi\n", mail.id);
    } else {
        Serial.printf("[MailQueue] ✗ Mail #%d gönderilemedi: %s\n", mail.id, errorMessage.c_str());
    }
    
    return success;
}

void MailAgent::processQueue() {
    // Çok sık işleme yapma
    if (millis() - lastQueueProcess < QUEUE_PROCESS_INTERVAL) {
        return;
    }
    lastQueueProcess = millis();
    
    if (mailQueue.empty()) {
        return;
    }
    
    uint32_t now = millis();
    bool queueChanged = false;
    
    // ===== KUYRUK YÖNETİMİ =====
    // NOT: Gönderilmemiş mailler ASLA silinmez!
    // Sadece kuyruk çok büyükse (20+) VE en eski mail SKIPPED aşamasındaysa uyarı ver
    // Kullanıcı clearQueue() ile manuel temizleyebilir
    if (mailQueue.size() > MAX_QUEUE_SIZE) {
        Serial.printf("[MailQueue] ⚠️ Kuyruk dolu! %d mail bekliyor (max %d)\n", mailQueue.size(), MAX_QUEUE_SIZE);
        // Silmiyoruz - kullanıcı clearQueue() ile temizleyebilir
    }
    
    // WiFi kontrolü
    if (!netManager || !netManager->isConnected()) {
        if (queueChanged) saveQueueToStorage();
        return; // WiFi yok, bekle
    }
    
    // Sırayla işle (priority order zaten sort edilmiş)
    for (auto it = mailQueue.begin(); it != mailQueue.end(); ) {
        QueuedMail &mail = *it;
        
        // Henüz zamanı gelmemiş mi?
        // millis() overflow koruması: (now - nextRetryTime) negatif olursa overflow
        uint32_t elapsed = now - mail.nextRetryTime;
        if (elapsed > 0x7FFFFFFF) { // Overflow durumu - henüz zamanı gelmemiş
            ++it;
            continue;
        }
        
        String errorMessage;
        bool success = trySendQueuedMail(mail, errorMessage);
        
        if (success) {
            // Başarılı - kuyruktan çıkar
            it = mailQueue.erase(it);
            queueChanged = true;
            
            // SKIPPED mailler varsa, bir sonrakine geç
            // (Başarılı gönderimden sonra SKIPPED'ları da dene)
            for (auto &m : mailQueue) {
                if (m.phase == RetryPhase::SKIPPED) {
                    m.nextRetryTime = now; // Hemen dene
                }
            }
        } else {
            // Başarısız - retry phase ilerlet
            advanceRetryPhase(mail);
            queueChanged = true;
            
            // SKIPPED aşamasına geçtiyse, bir sonraki maile geç
            if (mail.phase == RetryPhase::SKIPPED && it != mailQueue.end()) {
                // Sonraki maillerin hemen denenmesini sağla
                auto nextIt = it;
                ++nextIt;
                if (nextIt != mailQueue.end()) {
                    nextIt->nextRetryTime = now;
                }
            }
            
            ++it;
        }
        
        // Bir seferde sadece bir mail işle (sistem yükünü azalt)
        break;
    }
    
    if (queueChanged) {
        saveQueueToStorage();
    }
}