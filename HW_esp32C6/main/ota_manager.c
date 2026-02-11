/**
 * @file ota_manager.c
 * @brief OTA Update Implementation
 */

#include "ota_manager.h"
#include "log_manager.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG = "ota";

/* ============================================
 * PRIVATE DATA
 * ============================================ */

static ota_status_t s_status = {0};
static ota_progress_cb_t s_progress_cb = NULL;
static const esp_partition_t *s_update_partition = NULL;
static esp_ota_handle_t s_ota_handle = 0;

/* ============================================
 * PRIVATE FUNCTIONS
 * ============================================ */

static void set_state(ota_state_t state)
{
    s_status.state = state;
    
    if (state == OTA_STATE_FAILED) {
        LOG_SYSTEM(LOG_LEVEL_ERROR, "OTA failed: %s", s_status.error_message);
    }
}

static void update_progress(size_t received, size_t total)
{
    s_status.bytes_received = received;
    s_status.total_bytes = total;
    
    if (total > 0) {
        s_status.progress_percent = (int)((received * 100) / total);
        
        if (s_progress_cb) {
            s_progress_cb(s_status.progress_percent);
        }
    }
}

/* ============================================
 * INITIALIZATION
 * ============================================ */

esp_err_t ota_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager");
    
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = OTA_STATE_IDLE;
    
    // Log current partition
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running partition: %s at 0x%lx", 
                 running->label, running->address);
    }
    
    // Check for pending validation
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "Pending OTA verification, marking as valid");
            ota_mark_valid();
        }
    }
    
    return ESP_OK;
}

void ota_manager_set_progress_cb(ota_progress_cb_t cb)
{
    s_progress_cb = cb;
}

/* ============================================
 * OTA FROM URL
 * ============================================ */

esp_err_t ota_start_from_url(const char *url)
{
    if (!url || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_status.state == OTA_STATE_DOWNLOADING || 
        s_status.state == OTA_STATE_WRITING) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting OTA from: %s", url);
    LOG_SYSTEM(LOG_LEVEL_INFO, "OTA started from URL");
    
    set_state(OTA_STATE_DOWNLOADING);
    memset(s_status.error_message, 0, sizeof(s_status.error_message));
    
    // Find update partition
    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_update_partition) {
        strncpy(s_status.error_message, "No update partition", sizeof(s_status.error_message) - 1);
        set_state(OTA_STATE_FAILED);
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Target partition: %s", s_update_partition->label);
    
    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        strncpy(s_status.error_message, "HTTP client init failed", sizeof(s_status.error_message) - 1);
        set_state(OTA_STATE_FAILED);
        return ESP_FAIL;
    }
    
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        strncpy(s_status.error_message, "HTTP open failed", sizeof(s_status.error_message) - 1);
        esp_http_client_cleanup(client);
        set_state(OTA_STATE_FAILED);
        return ret;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        strncpy(s_status.error_message, "Invalid content length", sizeof(s_status.error_message) - 1);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        set_state(OTA_STATE_FAILED);
        return ESP_ERR_INVALID_SIZE;
    }
    
    ESP_LOGI(TAG, "Firmware size: %d bytes", content_length);
    s_status.total_bytes = content_length;
    
    // Begin OTA
    ret = esp_ota_begin(s_update_partition, OTA_SIZE_UNKNOWN, &s_ota_handle);
    if (ret != ESP_OK) {
        snprintf(s_status.error_message, sizeof(s_status.error_message), 
                 "OTA begin failed: %s", esp_err_to_name(ret));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        set_state(OTA_STATE_FAILED);
        return ret;
    }
    
    set_state(OTA_STATE_WRITING);
    
    // Download and write
    char buffer[4096];
    int total_read = 0;
    
    while (total_read < content_length) {
        int read = esp_http_client_read(client, buffer, sizeof(buffer));
        if (read <= 0) {
            break;
        }
        
        ret = esp_ota_write(s_ota_handle, buffer, read);
        if (ret != ESP_OK) {
            snprintf(s_status.error_message, sizeof(s_status.error_message),
                     "OTA write failed: %s", esp_err_to_name(ret));
            esp_ota_abort(s_ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            set_state(OTA_STATE_FAILED);
            return ret;
        }
        
        total_read += read;
        update_progress(total_read, content_length);
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    if (total_read != content_length) {
        strncpy(s_status.error_message, "Incomplete download", sizeof(s_status.error_message) - 1);
        esp_ota_abort(s_ota_handle);
        set_state(OTA_STATE_FAILED);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Verify and finish
    set_state(OTA_STATE_VERIFYING);
    
    ret = esp_ota_end(s_ota_handle);
    if (ret != ESP_OK) {
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "OTA verify failed: %s", esp_err_to_name(ret));
        set_state(OTA_STATE_FAILED);
        return ret;
    }
    
    // Set boot partition
    ret = esp_ota_set_boot_partition(s_update_partition);
    if (ret != ESP_OK) {
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "Set boot failed: %s", esp_err_to_name(ret));
        set_state(OTA_STATE_FAILED);
        return ret;
    }
    
    set_state(OTA_STATE_SUCCESS);
    LOG_SYSTEM(LOG_LEVEL_INFO, "OTA success, restart required");
    
    return ESP_OK;
}

esp_err_t ota_start_from_data(const uint8_t *data, size_t size)
{
    if (!data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Starting OTA from data (%zu bytes)", size);
    LOG_SYSTEM(LOG_LEVEL_INFO, "OTA started from upload");
    
    set_state(OTA_STATE_WRITING);
    memset(s_status.error_message, 0, sizeof(s_status.error_message));
    s_status.total_bytes = size;
    s_status.bytes_received = 0;
    
    // Find update partition
    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_update_partition) {
        strncpy(s_status.error_message, "No update partition", sizeof(s_status.error_message) - 1);
        set_state(OTA_STATE_FAILED);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Begin OTA
    esp_err_t ret = esp_ota_begin(s_update_partition, size, &s_ota_handle);
    if (ret != ESP_OK) {
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "OTA begin failed: %s", esp_err_to_name(ret));
        set_state(OTA_STATE_FAILED);
        return ret;
    }
    
    // Write data
    ret = esp_ota_write(s_ota_handle, data, size);
    if (ret != ESP_OK) {
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "OTA write failed: %s", esp_err_to_name(ret));
        esp_ota_abort(s_ota_handle);
        set_state(OTA_STATE_FAILED);
        return ret;
    }
    
    update_progress(size, size);
    
    // Verify and finish
    set_state(OTA_STATE_VERIFYING);
    
    ret = esp_ota_end(s_ota_handle);
    if (ret != ESP_OK) {
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "OTA verify failed: %s", esp_err_to_name(ret));
        set_state(OTA_STATE_FAILED);
        return ret;
    }
    
    // Set boot partition
    ret = esp_ota_set_boot_partition(s_update_partition);
    if (ret != ESP_OK) {
        snprintf(s_status.error_message, sizeof(s_status.error_message),
                 "Set boot failed: %s", esp_err_to_name(ret));
        set_state(OTA_STATE_FAILED);
        return ret;
    }
    
    set_state(OTA_STATE_SUCCESS);
    LOG_SYSTEM(LOG_LEVEL_INFO, "OTA success, restart required");
    
    return ESP_OK;
}

void ota_abort(void)
{
    if (s_ota_handle) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }
    set_state(OTA_STATE_IDLE);
    LOG_SYSTEM(LOG_LEVEL_WARN, "OTA aborted");
}

/* ============================================
 * STATUS
 * ============================================ */

void ota_get_status(ota_status_t *status)
{
    if (status) {
        *status = s_status;
    }
}

bool ota_is_in_progress(void)
{
    return (s_status.state == OTA_STATE_DOWNLOADING ||
            s_status.state == OTA_STATE_WRITING ||
            s_status.state == OTA_STATE_VERIFYING);
}

/* ============================================
 * FIRMWARE INFO
 * ============================================ */

void ota_get_version(char *buffer, size_t size)
{
    if (!buffer || size == 0) return;
    
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        snprintf(buffer, size, "%s", desc->version);
    } else {
        strncpy(buffer, "unknown", size - 1);
    }
}

void ota_get_partition_info(char *buffer, size_t size)
{
    if (!buffer || size == 0) return;
    
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        snprintf(buffer, size, "%s (0x%lx)", running->label, running->address);
    } else {
        strncpy(buffer, "unknown", size - 1);
    }
}

void ota_restart(void)
{
    LOG_SYSTEM(LOG_LEVEL_INFO, "Restarting for OTA update...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t ota_rollback(void)
{
    esp_err_t ret = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (ret != ESP_OK) {
        LOG_SYSTEM(LOG_LEVEL_ERROR, "Rollback failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t ota_mark_valid(void)
{
    esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "App marked as valid");
    }
    return ret;
}
