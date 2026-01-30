#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <vector>
#include "config_store.h"
#include "scheduler.h"
#include "network_manager.h"

// ============================================================================
// MAIL QUEUE YAPISI - Persistent, Never Expires
// ============================================================================

// Mail tipi önceliği: Warning > Final
enum class MailType : uint8_t {
    WARNING = 0,  // Yüksek öncelik
    FINAL = 1     // Düşük öncelik
};

// Retry aşaması
enum class RetryPhase : uint8_t {
    PHASE1 = 0,   // 5x60s (5 deneme, 60 saniye aralıklarla)
    PHASE2 = 1,   // 10x300s (10 deneme, 5 dakika aralıklarla)
    SKIPPED = 2   // Skip edildi, 600s infinite (sonraki maile geçildi, arka planda devam)
};

struct QueuedMail {
    uint32_t id;               // Unique ID
    MailType type;             // WARNING veya FINAL
    RetryPhase phase;          // Hangi retry aşamasında
    uint8_t attemptCount;      // Bu aşamada kaç deneme yapıldı
    uint32_t nextRetryTime;    // Sonraki deneme zamanı (millis)
    uint32_t createdAt;        // Oluşturulma zamanı (millis)
    
    // Mail içeriği
    String subject;
    String body;
    uint8_t alarmIndex;        // Warning için
    bool includeAttachments;
    
    // Scheduler snapshot (serialization için sadece gerekli alanlar)
    String startTime;
    String endTime;
    String description;
};

class MailAgent {
public:
    void begin(ConfigStore *storePtr, LebenSpurNetworkManager *netMgrPtr, const String &deviceIdStr);

    void updateConfig(const MailSettings &config);
    MailSettings currentConfig() const { return settings; }

    bool sendWarning(uint8_t alarmIndex, const ScheduleSnapshot &snapshot, String &errorMessage);
    bool sendFinal(const ScheduleSnapshot &snapshot, TimerRuntime &runtime, String &errorMessage);
    
    // Test fonksiyonları - sadece gönderen adrese mail atar
    bool sendWarningTest(const ScheduleSnapshot &snapshot, String &errorMessage);
    bool sendFinalTest(const ScheduleSnapshot &snapshot, String &errorMessage);

    // URL Validation - SSRF Koruması
    static bool isValidURL(const String &url);
    
    // ===== MAIL QUEUE API =====
    void processQueue();                    // Ana loop'tan çağrılır, kuyruk işler
    bool hasQueuedMails() const;            // Kuyrukta mail var mı?
    size_t getQueueSize() const;            // Kuyruk boyutu
    void loadQueueFromStorage();            // LittleFS'ten yükle (begin'de çağrılır)
    void saveQueueToStorage();              // LittleFS'e kaydet
    void clearQueue();                      // Kuyruğu temizle (debug için)

private:
    ConfigStore *store = nullptr;
    LebenSpurNetworkManager *netManager = nullptr;
    MailSettings settings;
    String deviceId;
    
    // ===== MAIL QUEUE =====
    std::vector<QueuedMail> mailQueue;
    uint32_t nextMailId = 1;
    uint32_t lastQueueProcess = 0;
    static constexpr uint32_t QUEUE_PROCESS_INTERVAL = 10000; // 10 saniyede bir kontrol
    static constexpr const char* QUEUE_FILE = "/mail_queue.json";
    static constexpr size_t MAX_QUEUE_SIZE = 20;  // Maksimum kuyruk boyutu (20 mail)
    // NOT: Gönderilmemiş mailler ASLA silinmez - kullanıcı değiştirmedikçe kalır
    
    // Queue helpers
    void enqueueWarning(uint8_t alarmIndex, const ScheduleSnapshot &snapshot);
    void enqueueFinal(const ScheduleSnapshot &snapshot, TimerRuntime &runtime);
    bool trySendQueuedMail(QueuedMail &mail, String &errorMessage);
    void advanceRetryPhase(QueuedMail &mail);
    uint32_t getRetryInterval(RetryPhase phase) const;
    void sortQueueByPriority();

    bool sendEmail(const String &subject, const String &body, bool includeWarningAttachments, String &errorMessage);
    bool sendEmailToSelf(const String &subject, const String &body, bool includeWarningAttachments, String &errorMessage); // Test için
    bool sendEmailToRecipient(const String &recipient, const String &subject, const String &body, bool includeWarningAttachments, String &errorMessage); // LebensSpur protokolü için
    bool smtpConnect(WiFiClientSecure &client, String &errorMessage);
    bool smtpAuth(WiFiClientSecure &client, String &errorMessage);
    bool smtpSendMail(WiFiClientSecure &client, const String &subject, const String &body, bool includeAttachments, String &errorMessage);
    bool smtpCommand(WiFiClientSecure &client, const String &command, const String &expectCode, String &errorMessage);
    String smtpReadLine(WiFiClientSecure &client, uint32_t timeoutMs = 5000);
    String base64Encode(const String &input);
    String buildMimeMessage(const String &subject, const String &body, bool includeWarningAttachments);
    void appendAttachments(String &mime, const String &boundary, bool warning); // DEPRECATED
    void smtpStreamAttachment(WiFiClientSecure &client, const String &boundary, const AttachmentMeta &meta); // RAM-efficient streaming
    String formatHeader() const;
    String formatElapsed(const ScheduleSnapshot &snapshot) const;
};
