/**
 * @file mail_sender.c
 * @brief Email Sender Implementation
 * 
 * Uses raw socket + TLS for SMTP communication.
 */

#include "mail_sender.h"
#include "config_manager.h"
#include "device_id.h"
#include "log_manager.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

static const char *TAG = "mail";

/* ============================================
 * SMTP PROTOCOL
 * ============================================ */

#define SMTP_TIMEOUT_SEC    10
#define SMTP_BUFFER_SIZE    1024

static int smtp_socket = -1;
static esp_tls_t *smtp_tls = NULL;

static int smtp_read(char *buffer, size_t size)
{
    memset(buffer, 0, size);
    
    if (smtp_tls) {
        return esp_tls_conn_read(smtp_tls, buffer, size - 1);
    } else if (smtp_socket >= 0) {
        return recv(smtp_socket, buffer, size - 1, 0);
    }
    return -1;
}

static int smtp_write(const char *data)
{
    size_t len = strlen(data);
    ESP_LOGD(TAG, "SMTP TX: %.*s", (int)(len > 50 ? 50 : len), data);
    
    if (smtp_tls) {
        return esp_tls_conn_write(smtp_tls, data, len);
    } else if (smtp_socket >= 0) {
        return send(smtp_socket, data, len, 0);
    }
    return -1;
}

static bool smtp_check_response(const char *expected_prefix)
{
    char buffer[SMTP_BUFFER_SIZE];
    int len = smtp_read(buffer, sizeof(buffer));
    
    if (len <= 0) {
        ESP_LOGE(TAG, "No response from server");
        return false;
    }
    
    ESP_LOGD(TAG, "SMTP RX: %s", buffer);
    
    return strncmp(buffer, expected_prefix, strlen(expected_prefix)) == 0;
}

static void smtp_close(void)
{
    if (smtp_tls) {
        esp_tls_conn_destroy(smtp_tls);
        smtp_tls = NULL;
    }
    if (smtp_socket >= 0) {
        close(smtp_socket);
        smtp_socket = -1;
    }
}

static esp_err_t smtp_connect(const char *server, uint16_t port, bool use_tls)
{
    ESP_LOGI(TAG, "Connecting to %s:%d (TLS=%d)", server, port, use_tls);
    
    if (use_tls || port == 465) {
        // Direct TLS (port 465)
        esp_tls_cfg_t tls_cfg = {
            .timeout_ms = SMTP_TIMEOUT_SEC * 1000,
            .skip_common_name = true,  // Allow any server cert
        };
        
        smtp_tls = esp_tls_init();
        if (!smtp_tls) {
            ESP_LOGE(TAG, "TLS init failed");
            return ESP_FAIL;
        }
        
        if (esp_tls_conn_new_sync(server, strlen(server), port, &tls_cfg, smtp_tls) != 1) {
            ESP_LOGE(TAG, "TLS connection failed");
            esp_tls_conn_destroy(smtp_tls);
            smtp_tls = NULL;
            return ESP_FAIL;
        }
        
    } else {
        // Plain connection (with optional STARTTLS)
        struct hostent *host = gethostbyname(server);
        if (!host) {
            ESP_LOGE(TAG, "DNS lookup failed for %s", server);
            return ESP_ERR_NOT_FOUND;
        }
        
        smtp_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (smtp_socket < 0) {
            ESP_LOGE(TAG, "Socket create failed");
            return ESP_FAIL;
        }
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        memcpy(&addr.sin_addr, host->h_addr, host->h_length);
        
        // Set timeout
        struct timeval tv;
        tv.tv_sec = SMTP_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(smtp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(smtp_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        if (connect(smtp_socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ESP_LOGE(TAG, "Connect failed");
            close(smtp_socket);
            smtp_socket = -1;
            return ESP_FAIL;
        }
    }
    
    // Read server greeting
    if (!smtp_check_response("220")) {
        ESP_LOGE(TAG, "Bad server greeting");
        smtp_close();
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

static esp_err_t smtp_auth(const char *username, const char *password)
{
    char buffer[512];
    
    // EHLO
    snprintf(buffer, sizeof(buffer), "EHLO %s\r\n", device_id_get());
    smtp_write(buffer);
    if (!smtp_check_response("250")) {
        return ESP_FAIL;
    }
    
    // AUTH LOGIN
    smtp_write("AUTH LOGIN\r\n");
    if (!smtp_check_response("334")) {
        ESP_LOGE(TAG, "AUTH not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // Username (base64)
    size_t out_len;
    unsigned char encoded[256];
    mbedtls_base64_encode(encoded, sizeof(encoded), &out_len, 
                          (const unsigned char *)username, strlen(username));
    snprintf(buffer, sizeof(buffer), "%s\r\n", encoded);
    smtp_write(buffer);
    if (!smtp_check_response("334")) {
        return ESP_FAIL;
    }
    
    // Password (base64)
    mbedtls_base64_encode(encoded, sizeof(encoded), &out_len,
                          (const unsigned char *)password, strlen(password));
    snprintf(buffer, sizeof(buffer), "%s\r\n", encoded);
    smtp_write(buffer);
    if (!smtp_check_response("235")) {
        ESP_LOGE(TAG, "Authentication failed");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "SMTP authenticated");
    return ESP_OK;
}

/* ============================================
 * PUBLIC API
 * ============================================ */

esp_err_t mail_sender_init(void)
{
    ESP_LOGI(TAG, "Mail sender initialized");
    return ESP_OK;
}

void mail_sender_deinit(void)
{
    smtp_close();
}

esp_err_t mail_send(const mail_server_config_t *config, const mail_message_t *message)
{
    if (!config || !message) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!config->smtp_server || !config->smtp_username || !config->smtp_password) {
        LOG_MAIL(LOG_LEVEL_ERROR, "SMTP not configured");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (message->recipient_count <= 0 || !message->recipients[0]) {
        LOG_MAIL(LOG_LEVEL_ERROR, "No recipients");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    
    // Connect
    ret = smtp_connect(config->smtp_server, config->smtp_port, config->use_tls);
    if (ret != ESP_OK) {
        LOG_MAIL(LOG_LEVEL_ERROR, "SMTP connect failed");
        return ret;
    }
    
    // Authenticate
    ret = smtp_auth(config->smtp_username, config->smtp_password);
    if (ret != ESP_OK) {
        smtp_close();
        LOG_MAIL(LOG_LEVEL_ERROR, "SMTP auth failed");
        return ret;
    }
    
    char buffer[1024];
    const char *sender_email = config->sender_email ? config->sender_email : config->smtp_username;
    const char *sender_name = config->sender_name ? config->sender_name : "LebensSpur";
    
    // MAIL FROM
    snprintf(buffer, sizeof(buffer), "MAIL FROM:<%s>\r\n", sender_email);
    smtp_write(buffer);
    if (!smtp_check_response("250")) {
        smtp_close();
        return ESP_FAIL;
    }
    
    // RCPT TO for each recipient
    for (int i = 0; i < message->recipient_count && message->recipients[i]; i++) {
        snprintf(buffer, sizeof(buffer), "RCPT TO:<%s>\r\n", message->recipients[i]);
        smtp_write(buffer);
        if (!smtp_check_response("250")) {
            ESP_LOGW(TAG, "Recipient rejected: %s", message->recipients[i]);
        }
    }
    
    // DATA
    smtp_write("DATA\r\n");
    if (!smtp_check_response("354")) {
        smtp_close();
        return ESP_FAIL;
    }
    
    // Headers
    snprintf(buffer, sizeof(buffer), 
             "From: %s <%s>\r\n"
             "Subject: %s\r\n"
             "MIME-Version: 1.0\r\n"
             "Content-Type: text/plain; charset=UTF-8\r\n"
             "X-Device-ID: %s\r\n",
             sender_name, sender_email,
             message->subject ? message->subject : "(no subject)",
             device_id_get());
    smtp_write(buffer);
    
    // To header
    smtp_write("To: ");
    for (int i = 0; i < message->recipient_count && message->recipients[i]; i++) {
        if (i > 0) smtp_write(", ");
        smtp_write(message->recipients[i]);
    }
    smtp_write("\r\n\r\n");
    
    // Body
    if (message->body) {
        smtp_write(message->body);
    }
    smtp_write("\r\n.\r\n");
    
    if (!smtp_check_response("250")) {
        smtp_close();
        LOG_MAIL(LOG_LEVEL_ERROR, "Mail send failed");
        return ESP_FAIL;
    }
    
    // QUIT
    smtp_write("QUIT\r\n");
    smtp_close();
    
    LOG_MAIL(LOG_LEVEL_INFO, "Mail sent to %d recipients", message->recipient_count);
    return ESP_OK;
}

esp_err_t mail_send_group(int group_index)
{
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    if (group_index < 0 || group_index >= mail_cfg.group_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    mail_group_t *group = &mail_cfg.groups[group_index];
    if (!group->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    
    mail_server_config_t server = {
        .smtp_server = mail_cfg.smtp_server,
        .smtp_port = mail_cfg.smtp_port,
        .smtp_username = mail_cfg.smtp_username,
        .smtp_password = mail_cfg.smtp_password,
        .use_tls = mail_cfg.use_tls,
        .sender_name = mail_cfg.sender_name
    };
    
    mail_message_t message = {
        .recipient_count = group->recipient_count,
        .subject = group->subject,
        .body = group->body
    };
    
    for (int i = 0; i < group->recipient_count; i++) {
        message.recipients[i] = group->recipients[i];
    }
    
    return mail_send(&server, &message);
}

esp_err_t mail_send_notification(const char *subject, const char *body)
{
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    if (mail_cfg.smtp_server[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    
    mail_server_config_t server = {
        .smtp_server = mail_cfg.smtp_server,
        .smtp_port = mail_cfg.smtp_port,
        .smtp_username = mail_cfg.smtp_username,
        .smtp_password = mail_cfg.smtp_password,
        .use_tls = mail_cfg.use_tls,
        .sender_name = mail_cfg.sender_name
    };
    
    esp_err_t last_err = ESP_ERR_NOT_FOUND;
    int success_count = 0;
    
    for (int g = 0; g < mail_cfg.group_count; g++) {
        mail_group_t *group = &mail_cfg.groups[g];
        if (!group->enabled || group->recipient_count == 0) {
            continue;
        }
        
        mail_message_t message = {
            .recipient_count = group->recipient_count,
            .subject = subject,
            .body = body
        };
        
        for (int i = 0; i < group->recipient_count; i++) {
            message.recipients[i] = group->recipients[i];
        }
        
        esp_err_t ret = mail_send(&server, &message);
        if (ret == ESP_OK) {
            success_count++;
        } else {
            last_err = ret;
        }
    }
    
    return (success_count > 0) ? ESP_OK : last_err;
}

bool mail_is_configured(void)
{
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    return (mail_cfg.smtp_server[0] != '\0' &&
            mail_cfg.smtp_username[0] != '\0' &&
            mail_cfg.smtp_password[0] != '\0');
}

esp_err_t mail_test_connection(void)
{
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    esp_err_t ret = smtp_connect(mail_cfg.smtp_server, mail_cfg.smtp_port, mail_cfg.use_tls);
    if (ret == ESP_OK) {
        ret = smtp_auth(mail_cfg.smtp_username, mail_cfg.smtp_password);
        smtp_write("QUIT\r\n");
    }
    smtp_close();
    
    return ret;
}

esp_err_t mail_send_test(const char *recipient)
{
    if (!recipient) return ESP_ERR_INVALID_ARG;
    
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    mail_server_config_t server = {
        .smtp_server = mail_cfg.smtp_server,
        .smtp_port = mail_cfg.smtp_port,
        .smtp_username = mail_cfg.smtp_username,
        .smtp_password = mail_cfg.smtp_password,
        .use_tls = mail_cfg.use_tls,
        .sender_name = mail_cfg.sender_name
    };
    
    mail_message_t message = {
        .recipient_count = 1,
        .recipients = {recipient},
        .subject = "LebensSpur Test Email",
        .body = "This is a test email from your LebensSpur device.\n\n"
                "If you received this, email notifications are working correctly."
    };
    
    return mail_send(&server, &message);
}
