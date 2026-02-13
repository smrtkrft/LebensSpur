/**
 * Mail Sender - SMTP TLS Mail Gönderici
 *
 * SMTPS (port 465, implicit TLS) ile mail gönderimi.
 * FreeRTOS task ile asenkron kuyruk, şablon bazlı mesajlar.
 *
 * Bağımlılık: config_manager (Katman 2), wifi_manager (Katman 3)
 * Katman: 3 (İletişim)
 */

#ifndef MAIL_SENDER_H
#define MAIL_SENDER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mail türleri
typedef enum {
    MAIL_TYPE_TEST = 0,
    MAIL_TYPE_WARNING,      // Timer yaklaşıyor
    MAIL_TYPE_ALARM,        // Timer tetiklendi
    MAIL_TYPE_RESET,        // Timer sıfırlandı
    MAIL_TYPE_STATUS,       // Durum raporu
    MAIL_TYPE_CUSTOM,
} mail_type_t;

// Mail önceliği
typedef enum {
    MAIL_PRIORITY_LOW = 5,
    MAIL_PRIORITY_NORMAL = 3,
    MAIL_PRIORITY_HIGH = 1,
} mail_priority_t;

// Mail mesajı
typedef struct {
    const char *to;             // Alıcı (virgülle ayrılmış)
    const char *subject;
    const char *body;
    bool is_html;
    mail_priority_t priority;
    mail_type_t type;
} mail_message_t;

// Gönderim sonucu
typedef struct {
    bool success;
    int smtp_code;
    char error_msg[128];
    uint32_t send_time_ms;
} mail_result_t;

// Callback
typedef void (*mail_sent_cb_t)(mail_result_t *result, void *user_data);

/**
 * Mail sistemini başlat (config'den SMTP ayarları yüklenir)
 */
esp_err_t mail_sender_init(void);

/**
 * Mail sistemini durdur
 */
esp_err_t mail_sender_deinit(void);

/**
 * Mail gönder (asenkron, kuyruk)
 */
esp_err_t mail_send_async(const mail_message_t *msg, mail_sent_cb_t cb, void *user_data);

/**
 * Mail gönder (senkron, blocking)
 */
esp_err_t mail_send(const mail_message_t *msg, mail_result_t *result);

/**
 * Şablon bazlı mail gönderim
 */
esp_err_t mail_send_test(const char *to);
esp_err_t mail_send_warning(const char *to, uint32_t remaining_minutes);
esp_err_t mail_send_alarm(const char *to);
esp_err_t mail_send_reset_notification(const char *to);
esp_err_t mail_send_daily_status(const char *to);

/**
 * Gruba mail gönder
 */
esp_err_t mail_send_to_group(int group_index, mail_type_t type);

/**
 * Tüm gruplara mail gönder
 */
esp_err_t mail_send_to_all_groups(mail_type_t type);

/**
 * SMTP bağlantı testi
 */
esp_err_t mail_test_connection(mail_result_t *result);

/**
 * Kuyrukta bekleyen mail sayısı
 */
int mail_get_queue_count(void);

// İstatistikler
typedef struct {
    uint32_t total_sent;
    uint32_t total_failed;
    uint32_t last_send_time;
    uint32_t queue_count;
} mail_stats_t;

esp_err_t mail_get_stats(mail_stats_t *stats);
void mail_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // MAIL_SENDER_H
