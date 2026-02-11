/**
 * @file mail_sender.h
 * @brief Email Sender (SMTP Client)
 * 
 * Simple SMTP client for sending notification emails.
 * Supports TLS via ESP-TLS.
 */

#ifndef MAIL_SENDER_H
#define MAIL_SENDER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * CONFIGURATION
 * ============================================ */
#define MAIL_MAX_RECIPIENTS     8
#define MAIL_MAX_SUBJECT_LEN    128
#define MAIL_MAX_BODY_LEN       1024
#define MAIL_SEND_TIMEOUT_MS    30000

/* ============================================
 * STRUCTURES
 * ============================================ */

typedef struct {
    // SMTP Server
    const char *smtp_server;
    uint16_t smtp_port;
    const char *smtp_username;
    const char *smtp_password;
    bool use_tls;
    
    // Sender
    const char *sender_name;
    const char *sender_email;  // Uses smtp_username if NULL
} mail_server_config_t;

typedef struct {
    const char *recipients[MAIL_MAX_RECIPIENTS];
    int recipient_count;
    const char *subject;
    const char *body;
} mail_message_t;

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize mail sender
 */
esp_err_t mail_sender_init(void);

/**
 * @brief Deinitialize mail sender
 */
void mail_sender_deinit(void);

/* ============================================
 * SENDING
 * ============================================ */

/**
 * @brief Send email
 * @param config Server configuration
 * @param message Message to send
 * @return ESP_OK on success
 */
esp_err_t mail_send(const mail_server_config_t *config, const mail_message_t *message);

/**
 * @brief Send email using config from config_manager
 * @param group_index Mail group index
 * @return ESP_OK on success
 */
esp_err_t mail_send_group(int group_index);

/**
 * @brief Send quick notification to all enabled groups
 * @param subject Email subject
 * @param body Email body
 * @return ESP_OK if at least one succeeded
 */
esp_err_t mail_send_notification(const char *subject, const char *body);

/* ============================================
 * STATUS
 * ============================================ */

/**
 * @brief Check if mail is configured
 */
bool mail_is_configured(void);

/**
 * @brief Test SMTP connection
 * @return ESP_OK if connection successful
 */
esp_err_t mail_test_connection(void);

/**
 * @brief Send test email
 * @param recipient Test recipient email
 * @return ESP_OK if sent
 */
esp_err_t mail_send_test(const char *recipient);

#ifdef __cplusplus
}
#endif

#endif // MAIL_SENDER_H
