/**
 * Mail Sender - SMTP TLS Mail Gönderici
 *
 * SMTPS (port 465, implicit TLS) üzerinden mail gönderimi.
 * FreeRTOS task ile asenkron kuyruk sistemi.
 *
 * Bağımlılık: config_manager
 * Katman: 3 (İletişim)
 */

#include "mail_sender.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "MAIL";

#define SMTP_TIMEOUT_MS     15000
#define SMTP_BUF_SIZE       1024
#define MAIL_QUEUE_SIZE     5
#define MAIL_TASK_STACK     8192
#define MAIL_TASK_PRIO      5

// Kuyruk öğesi - pointer tabanlı (büyük struct kuyrukta tutmak yerine)
typedef struct {
    char to[256];
    char subject[128];
    char body[2048];
    bool is_html;
    mail_priority_t priority;
    mail_sent_cb_t callback;
    void *user_data;
} mail_queue_item_t;

static mail_config_t s_config;
static mail_group_t s_groups[MAX_MAIL_GROUPS];
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;
static mail_stats_t s_stats = {0};

// Forward declarations
static void mail_task_fn(void *param);
static esp_err_t smtp_send_one(const mail_queue_item_t *item, mail_result_t *result);

// ============================================================================
// Base64 Helper
// ============================================================================

static void b64_encode(const char *in, char *out, size_t out_size)
{
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, out_size, &olen,
                          (const unsigned char *)in, strlen(in));
    out[olen] = '\0';
}

// ============================================================================
// SMTP Protocol (TLS only, port 465)
// ============================================================================

static esp_err_t smtp_read(esp_tls_t *tls, char *buf, size_t size)
{
    memset(buf, 0, size);
    int len = esp_tls_conn_read(tls, buf, size - 1);
    if (len < 0) {
        ESP_LOGE(TAG, "SMTP okuma hatasi");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t smtp_write(esp_tls_t *tls, const char *data)
{
    int len = esp_tls_conn_write(tls, data, strlen(data));
    if (len < 0) {
        ESP_LOGE(TAG, "SMTP yazma hatasi");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t smtp_cmd(esp_tls_t *tls, const char *cmd, char *resp, size_t resp_size, char expect)
{
    if (smtp_write(tls, cmd) != ESP_OK) return ESP_FAIL;
    if (smtp_read(tls, resp, resp_size) != ESP_OK) return ESP_FAIL;

    if (resp[0] != expect) {
        ESP_LOGE(TAG, "SMTP beklenmeyen yanit: %c (beklenen: %c) -> %.64s", resp[0], expect, resp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t smtp_send_one(const mail_queue_item_t *item, mail_result_t *result)
{
    char resp[SMTP_BUF_SIZE];
    char cmd[SMTP_BUF_SIZE];
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // TLS bağlantısı (implicit TLS, port 465)
    esp_tls_cfg_t cfg = {
        .timeout_ms = SMTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        snprintf(result->error_msg, sizeof(result->error_msg), "TLS init hatasi");
        return ESP_FAIL;
    }

    if (esp_tls_conn_new_sync(s_config.server, strlen(s_config.server),
                               s_config.port, &cfg, tls) != 1) {
        snprintf(result->error_msg, sizeof(result->error_msg), "TLS baglanti hatasi: %s:%d",
                 s_config.server, s_config.port);
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }

    // Sunucu selamlama
    if (smtp_read(tls, resp, sizeof(resp)) != ESP_OK || resp[0] != '2') {
        snprintf(result->error_msg, sizeof(result->error_msg), "SMTP selamlama hatasi");
        goto fail;
    }

    // EHLO
    snprintf(cmd, sizeof(cmd), "EHLO lebensspur\r\n");
    if (smtp_cmd(tls, cmd, resp, sizeof(resp), '2') != ESP_OK) {
        snprintf(result->error_msg, sizeof(result->error_msg), "EHLO hatasi");
        goto fail;
    }

    // AUTH LOGIN
    if (smtp_cmd(tls, "AUTH LOGIN\r\n", resp, sizeof(resp), '3') != ESP_OK) {
        snprintf(result->error_msg, sizeof(result->error_msg), "AUTH hatasi");
        goto fail;
    }

    // Username (base64)
    char b64[128];
    b64_encode(s_config.username, b64, sizeof(b64));
    snprintf(cmd, sizeof(cmd), "%s\r\n", b64);
    if (smtp_cmd(tls, cmd, resp, sizeof(resp), '3') != ESP_OK) {
        snprintf(result->error_msg, sizeof(result->error_msg), "Username hatasi");
        goto fail;
    }

    // Password (base64)
    b64_encode(s_config.password, b64, sizeof(b64));
    snprintf(cmd, sizeof(cmd), "%s\r\n", b64);
    if (smtp_cmd(tls, cmd, resp, sizeof(resp), '2') != ESP_OK) {
        snprintf(result->error_msg, sizeof(result->error_msg), "Sifre hatasi");
        goto fail;
    }

    // MAIL FROM
    snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>\r\n", s_config.username);
    if (smtp_cmd(tls, cmd, resp, sizeof(resp), '2') != ESP_OK) {
        snprintf(result->error_msg, sizeof(result->error_msg), "MAIL FROM hatasi");
        goto fail;
    }

    // RCPT TO (virgülle ayrılmış alıcılar)
    {
        char to_copy[256];
        strncpy(to_copy, item->to, sizeof(to_copy) - 1);
        to_copy[sizeof(to_copy) - 1] = '\0';

        char *tok = strtok(to_copy, ",;");
        while (tok) {
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && *end == ' ') *end-- = '\0';

            snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", tok);
            smtp_cmd(tls, cmd, resp, sizeof(resp), '2');  // Bazı alıcılar reddedilse bile devam
            tok = strtok(NULL, ",;");
        }
    }

    // DATA
    if (smtp_cmd(tls, "DATA\r\n", resp, sizeof(resp), '3') != ESP_OK) {
        snprintf(result->error_msg, sizeof(result->error_msg), "DATA hatasi");
        goto fail;
    }

    // Mail içeriği
    {
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        char date_str[64];
        strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S +0100", &ti);

        char *mail = malloc(4096);
        if (!mail) {
            snprintf(result->error_msg, sizeof(result->error_msg), "Bellek hatasi");
            goto fail;
        }

        int len = snprintf(mail, 4096,
            "From: %s <%s>\r\n"
            "To: %s\r\n"
            "Subject: %s\r\n"
            "Date: %s\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: %s; charset=UTF-8\r\n"
            "X-Priority: %d\r\n"
            "X-Mailer: LebensSpur ESP32-C6\r\n"
            "\r\n"
            "%s\r\n"
            ".\r\n",
            s_config.sender_name[0] ? s_config.sender_name : "LebensSpur",
            s_config.username,
            item->to,
            item->subject,
            date_str,
            item->is_html ? "text/html" : "text/plain",
            item->priority,
            item->body);

        int written = esp_tls_conn_write(tls, mail, len);
        free(mail);

        if (written < 0) {
            snprintf(result->error_msg, sizeof(result->error_msg), "Mail yazma hatasi");
            goto fail;
        }
    }

    // Sonuç
    if (smtp_read(tls, resp, sizeof(resp)) != ESP_OK || resp[0] != '2') {
        snprintf(result->error_msg, sizeof(result->error_msg), "Mail reddedildi");
        result->smtp_code = atoi(resp);
        goto fail;
    }

    // QUIT
    smtp_write(tls, "QUIT\r\n");
    esp_tls_conn_destroy(tls);

    result->success = true;
    result->smtp_code = atoi(resp);
    result->send_time_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - start;

    ESP_LOGI(TAG, "Mail gonderildi: %s (%lu ms)", item->to, result->send_time_ms);
    return ESP_OK;

fail:
    smtp_write(tls, "QUIT\r\n");
    esp_tls_conn_destroy(tls);
    return ESP_FAIL;
}

// ============================================================================
// Mail Task
// ============================================================================

static void mail_task_fn(void *param)
{
    mail_queue_item_t *item = malloc(sizeof(mail_queue_item_t));
    if (!item) {
        ESP_LOGE(TAG, "Task bellek hatasi");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xQueueReceive(s_queue, item, portMAX_DELAY) == pdTRUE) {
            mail_result_t result = {0};

            if (smtp_send_one(item, &result) == ESP_OK) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_stats.total_sent++;
                s_stats.last_send_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                xSemaphoreGive(s_mutex);
            } else {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_stats.total_failed++;
                xSemaphoreGive(s_mutex);
            }

            if (item->callback) {
                item->callback(&result, item->user_data);
            }
        }
    }
}

// ============================================================================
// Mail Şablonları
// ============================================================================

static const char *get_subject(mail_type_t type, uint32_t param)
{
    static char buf[128];
    switch (type) {
        case MAIL_TYPE_TEST:
            snprintf(buf, sizeof(buf), "[LebensSpur] Test Maili");
            break;
        case MAIL_TYPE_WARNING:
            snprintf(buf, sizeof(buf), "[LebensSpur] UYARI - %lu dakika kaldi!", param);
            break;
        case MAIL_TYPE_ALARM:
            snprintf(buf, sizeof(buf), "[LebensSpur] ALARM - Timer Tetiklendi!");
            break;
        case MAIL_TYPE_RESET:
            snprintf(buf, sizeof(buf), "[LebensSpur] Timer Sifirlandi");
            break;
        case MAIL_TYPE_STATUS:
            snprintf(buf, sizeof(buf), "[LebensSpur] Durum Raporu");
            break;
        default:
            snprintf(buf, sizeof(buf), "[LebensSpur] Bilgilendirme");
            break;
    }
    return buf;
}

static const char *get_body(mail_type_t type, uint32_t param)
{
    static char buf[1024];
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &ti);

    switch (type) {
        case MAIL_TYPE_TEST:
            snprintf(buf, sizeof(buf),
                "LebensSpur Test Maili\n"
                "=====================\n\n"
                "Mail sistemi dogru calisiyor.\n\n"
                "Tarih: %s\nCihaz: ESP32-C6\n", ts);
            break;
        case MAIL_TYPE_WARNING:
            snprintf(buf, sizeof(buf),
                "LEBENSSPUR UYARI\n"
                "================\n\n"
                "Timer'i sifirlamaniz icin %lu dakikaniz kaldi!\n\n"
                "Sifirlama yapilmazsa alarm tetiklenecektir.\n\n"
                "Tarih: %s\n", param, ts);
            break;
        case MAIL_TYPE_ALARM:
            snprintf(buf, sizeof(buf),
                "LEBENSSPUR ALARM\n"
                "================\n\n"
                "TIMER TETIKLENDI!\n\n"
                "Belirlenen sure icinde sifirlama yapilmadi.\n"
                "Tanimli aksiyonlar uygulandi.\n\n"
                "Tarih: %s\n", ts);
            break;
        case MAIL_TYPE_RESET:
            snprintf(buf, sizeof(buf),
                "LebensSpur Timer Sifirlandi\n"
                "===========================\n\n"
                "Timer basariyla sifirlandi.\n"
                "Sistem normal calismaya devam ediyor.\n\n"
                "Tarih: %s\n", ts);
            break;
        case MAIL_TYPE_STATUS:
            snprintf(buf, sizeof(buf),
                "LebensSpur Durum Raporu\n"
                "=======================\n\n"
                "Sistem durumu: AKTIF\n\n"
                "Tarih: %s\n", ts);
            break;
        default:
            snprintf(buf, sizeof(buf), "LebensSpur Bilgilendirme\n\nTarih: %s\n", ts);
            break;
    }
    return buf;
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t mail_sender_init(void)
{
    if (s_initialized) return ESP_OK;

    config_load_mail(&s_config);

    // Mail gruplarını yükle
    for (int i = 0; i < MAX_MAIL_GROUPS; i++) {
        config_load_mail_group(i, &s_groups[i]);
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;

    s_queue = xQueueCreate(MAIL_QUEUE_SIZE, sizeof(mail_queue_item_t));
    if (!s_queue) {
        vSemaphoreDelete(s_mutex);
        return ESP_FAIL;
    }

    if (xTaskCreate(mail_task_fn, "mail", MAIL_TASK_STACK, NULL,
                     MAIL_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_mutex);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OK - %s:%d user=%s", s_config.server, s_config.port, s_config.username);
    return ESP_OK;
}

esp_err_t mail_sender_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
    if (s_mutex) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }

    s_initialized = false;
    return ESP_OK;
}

esp_err_t mail_send_async(const mail_message_t *msg, mail_sent_cb_t cb, void *user_data)
{
    if (!s_initialized || !msg || !msg->to) return ESP_ERR_INVALID_STATE;

    mail_queue_item_t item = {0};
    strncpy(item.to, msg->to, sizeof(item.to) - 1);
    if (msg->subject) strncpy(item.subject, msg->subject, sizeof(item.subject) - 1);
    if (msg->body) strncpy(item.body, msg->body, sizeof(item.body) - 1);
    item.is_html = msg->is_html;
    item.priority = msg->priority;
    item.callback = cb;
    item.user_data = user_data;

    if (xQueueSend(s_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Kuyruk dolu");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mail_send(const mail_message_t *msg, mail_result_t *result)
{
    if (!s_initialized || !msg || !result) return ESP_ERR_INVALID_STATE;

    memset(result, 0, sizeof(*result));

    mail_queue_item_t item = {0};
    strncpy(item.to, msg->to, sizeof(item.to) - 1);
    if (msg->subject) strncpy(item.subject, msg->subject, sizeof(item.subject) - 1);
    if (msg->body) strncpy(item.body, msg->body, sizeof(item.body) - 1);
    item.is_html = msg->is_html;
    item.priority = msg->priority;

    esp_err_t ret = smtp_send_one(&item, result);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ret == ESP_OK) {
        s_stats.total_sent++;
        s_stats.last_send_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    } else {
        s_stats.total_failed++;
    }
    xSemaphoreGive(s_mutex);

    return ret;
}

// Şablon bazlı gönderim fonksiyonları

esp_err_t mail_send_test(const char *to)
{
    mail_message_t msg = {
        .to = to, .subject = get_subject(MAIL_TYPE_TEST, 0),
        .body = get_body(MAIL_TYPE_TEST, 0),
        .priority = MAIL_PRIORITY_NORMAL, .type = MAIL_TYPE_TEST,
    };
    return mail_send_async(&msg, NULL, NULL);
}

esp_err_t mail_send_warning(const char *to, uint32_t remaining_minutes)
{
    mail_message_t msg = {
        .to = to, .subject = get_subject(MAIL_TYPE_WARNING, remaining_minutes),
        .body = get_body(MAIL_TYPE_WARNING, remaining_minutes),
        .priority = MAIL_PRIORITY_HIGH, .type = MAIL_TYPE_WARNING,
    };
    return mail_send_async(&msg, NULL, NULL);
}

esp_err_t mail_send_alarm(const char *to)
{
    mail_message_t msg = {
        .to = to, .subject = get_subject(MAIL_TYPE_ALARM, 0),
        .body = get_body(MAIL_TYPE_ALARM, 0),
        .priority = MAIL_PRIORITY_HIGH, .type = MAIL_TYPE_ALARM,
    };
    return mail_send_async(&msg, NULL, NULL);
}

esp_err_t mail_send_reset_notification(const char *to)
{
    mail_message_t msg = {
        .to = to, .subject = get_subject(MAIL_TYPE_RESET, 0),
        .body = get_body(MAIL_TYPE_RESET, 0),
        .priority = MAIL_PRIORITY_NORMAL, .type = MAIL_TYPE_RESET,
    };
    return mail_send_async(&msg, NULL, NULL);
}

esp_err_t mail_send_daily_status(const char *to)
{
    mail_message_t msg = {
        .to = to, .subject = get_subject(MAIL_TYPE_STATUS, 0),
        .body = get_body(MAIL_TYPE_STATUS, 0),
        .priority = MAIL_PRIORITY_LOW, .type = MAIL_TYPE_STATUS,
    };
    return mail_send_async(&msg, NULL, NULL);
}

esp_err_t mail_send_to_group(int group_index, mail_type_t type)
{
    if (group_index < 0 || group_index >= MAX_MAIL_GROUPS) return ESP_ERR_INVALID_ARG;
    if (!s_groups[group_index].enabled || s_groups[group_index].recipient_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    // Alıcı listesi oluştur
    char recipients[512] = {0};
    for (int i = 0; i < s_groups[group_index].recipient_count; i++) {
        if (i > 0) strcat(recipients, ",");
        strcat(recipients, s_groups[group_index].recipients[i]);
    }

    switch (type) {
        case MAIL_TYPE_TEST:    return mail_send_test(recipients);
        case MAIL_TYPE_WARNING: return mail_send_warning(recipients, 30);
        case MAIL_TYPE_ALARM:   return mail_send_alarm(recipients);
        case MAIL_TYPE_RESET:   return mail_send_reset_notification(recipients);
        case MAIL_TYPE_STATUS:  return mail_send_daily_status(recipients);
        default: return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t mail_send_to_all_groups(mail_type_t type)
{
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < MAX_MAIL_GROUPS; i++) {
        if (s_groups[i].enabled && s_groups[i].recipient_count > 0) {
            esp_err_t r = mail_send_to_group(i, type);
            if (r != ESP_OK) ret = r;
        }
    }
    return ret;
}

esp_err_t mail_test_connection(mail_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    esp_tls_cfg_t cfg = {
        .timeout_ms = SMTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        snprintf(result->error_msg, sizeof(result->error_msg), "TLS init hatasi");
        return ESP_FAIL;
    }

    if (esp_tls_conn_new_sync(s_config.server, strlen(s_config.server),
                               s_config.port, &cfg, tls) != 1) {
        snprintf(result->error_msg, sizeof(result->error_msg), "Baglanti hatasi");
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }

    // Selamlama
    char resp[SMTP_BUF_SIZE];
    smtp_read(tls, resp, sizeof(resp));

    // EHLO
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "EHLO lebensspur\r\n");
    smtp_cmd(tls, cmd, resp, sizeof(resp), '2');

    // AUTH LOGIN
    if (smtp_cmd(tls, "AUTH LOGIN\r\n", resp, sizeof(resp), '3') == ESP_OK) {
        char b64[128];
        b64_encode(s_config.username, b64, sizeof(b64));
        snprintf(cmd, sizeof(cmd), "%s\r\n", b64);
        smtp_cmd(tls, cmd, resp, sizeof(resp), '3');

        b64_encode(s_config.password, b64, sizeof(b64));
        snprintf(cmd, sizeof(cmd), "%s\r\n", b64);
        if (smtp_cmd(tls, cmd, resp, sizeof(resp), '2') == ESP_OK) {
            result->success = true;
            result->smtp_code = 250;
        } else {
            snprintf(result->error_msg, sizeof(result->error_msg), "Giris basarisiz");
        }
    } else {
        snprintf(result->error_msg, sizeof(result->error_msg), "AUTH desteklenmiyor");
    }

    smtp_write(tls, "QUIT\r\n");
    esp_tls_conn_destroy(tls);
    return result->success ? ESP_OK : ESP_FAIL;
}

int mail_get_queue_count(void)
{
    return s_queue ? uxQueueMessagesWaiting(s_queue) : 0;
}

esp_err_t mail_get_stats(mail_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *stats = s_stats;
    stats->queue_count = mail_get_queue_count();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void mail_print_stats(void)
{
    mail_stats_t st;
    mail_get_stats(&st);

    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ Server:    %s:%d", s_config.server, s_config.port);
    ESP_LOGI(TAG, "│ User:      %s", s_config.username);
    ESP_LOGI(TAG, "│ Gonderilen: %lu", st.total_sent);
    ESP_LOGI(TAG, "│ Basarisiz:  %lu", st.total_failed);
    ESP_LOGI(TAG, "│ Kuyrukta:   %lu", st.queue_count);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
