/**
 * OTA Manager - Over-The-Air Firmware Guncelleme
 *
 * URL (HTTPS) ile esp_https_ota, dosyadan esp_ota_ops kullanir.
 * Boot sonrasi PENDING_VERIFY durumunda otomatik onaylama.
 * Rollback destegi: onceki partition'a geri donus.
 */

#include "ota_manager.h"
#include "file_manager.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OTA";

#define OTA_BUFFER_SIZE     4096

static ota_state_t s_state = OTA_STATE_IDLE;
static uint8_t s_progress = 0;
static ota_progress_cb_t s_progress_cb = NULL;
static char s_version[32] = "unknown";

// ============================================================================
// Public API
// ============================================================================

esp_err_t ota_manager_init(void)
{
    // App descriptor'dan version al
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        strncpy(s_version, desc->version, sizeof(s_version) - 1);
        s_version[sizeof(s_version) - 1] = '\0';
    }

    // Calisan partition kontrolu
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Calisan partition alinamadi");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Partition: %s (0x%08lx), v%s",
             running->label, running->address, s_version);

    // PENDING_VERIFY durumunda otomatik onayla
    esp_ota_img_states_t img_state;
    if (esp_ota_get_state_partition(running, &img_state) == ESP_OK) {
        if (img_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "OTA dogrulama bekleniyor - otomatik onaylaniyor");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    s_state = OTA_STATE_IDLE;
    ESP_LOGI(TAG, "OK");
    return ESP_OK;
}

esp_err_t ota_manager_start_from_url(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;

    if (s_state != OTA_STATE_IDLE) {
        ESP_LOGW(TAG, "OTA zaten devam ediyor");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "URL OTA baslatiliyor: %s", url);
    s_state = OTA_STATE_DOWNLOADING;
    s_progress = 0;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .keep_alive_enable = true,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);

    if (ret == ESP_OK) {
        s_state = OTA_STATE_COMPLETE;
        s_progress = 100;
        ESP_LOGI(TAG, "URL OTA basarili");
    } else {
        s_state = OTA_STATE_ERROR;
        ESP_LOGE(TAG, "URL OTA basarisiz: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t ota_manager_start_from_file(const char *filepath)
{
    if (!filepath) return ESP_ERR_INVALID_ARG;

    if (s_state != OTA_STATE_IDLE) {
        ESP_LOGW(TAG, "OTA zaten devam ediyor");
        return ESP_ERR_INVALID_STATE;
    }

    if (!file_manager_exists(filepath)) {
        ESP_LOGE(TAG, "Firmware dosyasi bulunamadi: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Dosya OTA: %s", filepath);
    s_state = OTA_STATE_DOWNLOADING;
    s_progress = 0;

    // Hedef partition
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        s_state = OTA_STATE_ERROR;
        ESP_LOGE(TAG, "Update partition bulunamadi");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Hedef: %s (0x%08lx, %lu byte)",
             update->label, update->address, update->size);

    // Dosyayi ac
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        s_state = OTA_STATE_ERROR;
        ESP_LOGE(TAG, "Dosya acilamadi");
        return ESP_FAIL;
    }

    // Dosya boyutu
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || (uint32_t)file_size > update->size) {
        fclose(f);
        s_state = OTA_STATE_ERROR;
        ESP_LOGE(TAG, "Gecersiz dosya boyutu: %ld", file_size);
        return ESP_FAIL;
    }

    // OTA baslat
    esp_ota_handle_t handle;
    esp_err_t ret = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (ret != ESP_OK) {
        fclose(f);
        s_state = OTA_STATE_ERROR;
        ESP_LOGE(TAG, "OTA begin hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = OTA_STATE_UPDATING;

    // Buffer ayir
    uint8_t *buf = malloc(OTA_BUFFER_SIZE);
    if (!buf) {
        esp_ota_abort(handle);
        fclose(f);
        s_state = OTA_STATE_ERROR;
        return ESP_ERR_NO_MEM;
    }

    // Parca parca yaz
    long written = 0;
    size_t rd;

    while ((rd = fread(buf, 1, OTA_BUFFER_SIZE, f)) > 0) {
        ret = esp_ota_write(handle, buf, rd);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA write hatasi: %s", esp_err_to_name(ret));
            break;
        }

        written += rd;
        s_progress = (uint8_t)((written * 100) / file_size);

        if (s_progress_cb) {
            s_progress_cb(written, file_size);
        }

        // Her %10'da log
        if ((written * 10 / file_size) != ((written - rd) * 10 / file_size)) {
            ESP_LOGI(TAG, "Ilerleme: %d%%", s_progress);
        }
    }

    fclose(f);
    free(buf);

    if (ret != ESP_OK) {
        esp_ota_abort(handle);
        s_state = OTA_STATE_ERROR;
        return ret;
    }

    // Dogrulama
    s_state = OTA_STATE_VERIFYING;
    ret = esp_ota_end(handle);
    if (ret != ESP_OK) {
        s_state = OTA_STATE_ERROR;
        ESP_LOGE(TAG, "OTA end hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Boot partition ayarla
    ret = esp_ota_set_boot_partition(update);
    if (ret != ESP_OK) {
        s_state = OTA_STATE_ERROR;
        ESP_LOGE(TAG, "Boot partition ayarlanamadi: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = OTA_STATE_COMPLETE;
    s_progress = 100;
    ESP_LOGI(TAG, "Dosya OTA tamamlandi");

    return ESP_OK;
}

ota_state_t ota_manager_get_state(void)
{
    return s_state;
}

uint8_t ota_manager_get_progress(void)
{
    return s_progress;
}

esp_err_t ota_manager_abort(void)
{
    if (s_state == OTA_STATE_IDLE || s_state == OTA_STATE_COMPLETE) {
        return ESP_OK;
    }

    s_state = OTA_STATE_IDLE;
    s_progress = 0;
    ESP_LOGW(TAG, "OTA iptal edildi");
    return ESP_OK;
}

void ota_manager_restart(void)
{
    ESP_LOGI(TAG, "Yeniden baslatiliyor...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void ota_manager_set_progress_callback(ota_progress_cb_t callback)
{
    s_progress_cb = callback;
}

const char *ota_manager_get_current_version(void)
{
    return s_version;
}

esp_err_t ota_manager_rollback(void)
{
    ESP_LOGW(TAG, "Rollback yapiliyor...");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

esp_err_t ota_manager_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

void ota_manager_print_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ Firmware:  %s", s_version);

    if (running) {
        ESP_LOGI(TAG, "│ Calisan:   %s @ 0x%08lx", running->label, running->address);
    }
    if (next) {
        ESP_LOGI(TAG, "│ Sonraki:   %s @ 0x%08lx", next->label, next->address);
    }

    const char *state_str = "?";
    switch (s_state) {
        case OTA_STATE_IDLE:        state_str = "BOS"; break;
        case OTA_STATE_DOWNLOADING: state_str = "INDIRILIYOR"; break;
        case OTA_STATE_VERIFYING:   state_str = "DOGRULANIYOR"; break;
        case OTA_STATE_UPDATING:    state_str = "GUNCELLENIYOR"; break;
        case OTA_STATE_COMPLETE:    state_str = "TAMAMLANDI"; break;
        case OTA_STATE_ERROR:       state_str = "HATA"; break;
    }

    ESP_LOGI(TAG, "│ Durum:     %s (%d%%)", state_str, s_progress);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
