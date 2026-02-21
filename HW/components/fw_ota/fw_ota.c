/**
 * LebensSpur - Firmware OTA Guncelleme
 * GitHub Releases'tan firmware indirip dahili flash'a yazar
 * esp_https_ota ile A/B partition guncelleme + rollback destegi
 */

#include "fw_ota.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "FW_OTA";

// GitHub repo bilgileri
#define GITHUB_API_URL       "https://api.github.com/repos/smrtkrft/LebensSpur/releases/latest"
#define GITHUB_DL_TEMPLATE   "https://github.com/smrtkrft/LebensSpur/releases/download/%s/lebensspur.bin"

// Ayarlar
#define HTTP_TIMEOUT_MS      15000
#define OTA_TIMEOUT_MS       120000
#define CHECK_BUF_SIZE       4096
#define OTA_TASK_STACK       12288

// Durum
static fw_ota_status_t s_status;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task_handle = NULL;

// Check sonrasi saklanan indirme URL'i
static char s_download_url[300];

// Custom URL (check icin)
static char s_custom_url[300];

// Diger partition bilgisi (cache)
static char s_rollback_version[32];
static bool s_rollback_available = false;

// ============================================================
// Yardimci fonksiyonlar
// ============================================================

static void set_status(fw_ota_state_t state, const char *msg)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = state;
    if (msg) {
        strncpy(s_status.message, msg, sizeof(s_status.message) - 1);
        s_status.message[sizeof(s_status.message) - 1] = '\0';
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
}

static void set_progress(int pct)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.progress_pct = pct;
    if (s_mutex) xSemaphoreGive(s_mutex);
}

// Basit JSON string parser: "key":"value" veya "key" : "value" formatinda
static int parse_json_str(const char *json, const char *key, char *out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *key_pos = strstr(json, search);
    if (!key_pos) return -1;

    const char *colon = strchr(key_pos + strlen(search), ':');
    if (!colon) return -1;

    const char *q1 = strchr(colon + 1, '"');
    if (!q1) return -1;
    q1++;

    const char *q2 = strchr(q1, '"');
    if (!q2) return -1;

    size_t len = q2 - q1;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, q1, len);
    out[len] = '\0';
    return (int)len;
}

// Versiyon karsilastirma: "v0.0.67" -> "0.0.67"
static const char *strip_v_prefix(const char *ver)
{
    if (ver && (ver[0] == 'v' || ver[0] == 'V')) {
        return ver + 1;
    }
    return ver;
}

// Rollback bilgisini guncelle (cache)
static void update_rollback_info(void)
{
    s_rollback_available = false;
    s_rollback_version[0] = '\0';

    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (!other) return;

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(other, &desc) == ESP_OK) {
        s_rollback_available = true;
        strncpy(s_rollback_version, desc.version, sizeof(s_rollback_version) - 1);
        s_rollback_version[sizeof(s_rollback_version) - 1] = '\0';
    }
}

// ============================================================
// Check Task - GitHub'dan versiyon kontrol
// ============================================================

static void fw_ota_check_task(void *arg)
{
    ESP_LOGI(TAG, "Versiyon kontrol basladi");
    set_status(FW_OTA_CHECKING, "Versiyon kontrol ediliyor...");

    // Mevcut versiyon
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_status.current_version, app_desc->version, sizeof(s_status.current_version) - 1);
    if (s_mutex) xSemaphoreGive(s_mutex);

    // Custom URL varsa: versiyon kontrolu atla, direkt hazir
    if (s_custom_url[0] != '\0') {
        strncpy(s_download_url, s_custom_url, sizeof(s_download_url) - 1);
        s_download_url[sizeof(s_download_url) - 1] = '\0';

        if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
        strncpy(s_status.remote_version, "custom", sizeof(s_status.remote_version) - 1);
        if (s_mutex) xSemaphoreGive(s_mutex);

        ESP_LOGI(TAG, "Custom firmware URL: %s", s_download_url);
        set_status(FW_OTA_UPDATE_AVAILABLE, "Custom firmware hazir");
        goto done;
    }

    // Resmi: GitHub Releases API
    {
        esp_http_client_config_t config = {
            .url = GITHUB_API_URL,
            .method = HTTP_METHOD_GET,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = HTTP_TIMEOUT_MS,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            set_status(FW_OTA_ERROR, "HTTP client olusturulamadi");
            goto done;
        }

        // GitHub API User-Agent zorunlu
        esp_http_client_set_header(client, "User-Agent", "LebensSpur-ESP32");
        esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GitHub API baglanti hatasi: %s", esp_err_to_name(err));
            set_status(FW_OTA_ERROR, "Sunucuya baglanilamadi");
            esp_http_client_cleanup(client);
            goto done;
        }

        esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);

        if (status_code != 200) {
            ESP_LOGE(TAG, "GitHub API HTTP %d", status_code);
            char err_msg[64];
            snprintf(err_msg, sizeof(err_msg), "GitHub API hatasi: HTTP %d", status_code);
            set_status(FW_OTA_ERROR, err_msg);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            goto done;
        }

        // Yaniti oku (tag_name icin ilk 4KB yeterli)
        char *buf = malloc(CHECK_BUF_SIZE);
        if (!buf) {
            set_status(FW_OTA_ERROR, "Bellek yetersiz");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            goto done;
        }

        int total = 0;
        int len;
        while ((len = esp_http_client_read(client, buf + total,
                                            CHECK_BUF_SIZE - total - 1)) > 0) {
            total += len;
            if (total >= CHECK_BUF_SIZE - 1) break;
        }
        buf[total] = '\0';

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        // tag_name parse et
        char tag_name[32] = {0};
        if (parse_json_str(buf, "tag_name", tag_name, sizeof(tag_name)) < 0) {
            ESP_LOGE(TAG, "tag_name bulunamadi");
            set_status(FW_OTA_ERROR, "Release bilgisi okunamadi");
            free(buf);
            goto done;
        }
        free(buf);

        ESP_LOGI(TAG, "GitHub release tag: %s", tag_name);

        // Versiyon karsilastir
        const char *remote_ver = strip_v_prefix(tag_name);
        const char *current_ver = strip_v_prefix(app_desc->version);

        if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
        strncpy(s_status.remote_version, remote_ver,
                sizeof(s_status.remote_version) - 1);
        if (s_mutex) xSemaphoreGive(s_mutex);

        if (strcmp(current_ver, remote_ver) == 0) {
            ESP_LOGI(TAG, "Firmware guncel: v%s", current_ver);
            set_status(FW_OTA_NO_UPDATE, "Firmware guncel");
            goto done;
        }

        // Indirme URL'i olustur
        snprintf(s_download_url, sizeof(s_download_url),
                 GITHUB_DL_TEMPLATE, tag_name);
        ESP_LOGI(TAG, "Guncelleme mevcut: v%s -> v%s", current_ver, remote_ver);
        ESP_LOGI(TAG, "Indirme URL: %s", s_download_url);
        set_status(FW_OTA_UPDATE_AVAILABLE, "Guncelleme mevcut");
    }

done:
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================================
// Download Task - esp_https_ota ile firmware indir ve yaz
// ============================================================

static void fw_ota_download_task(void *arg)
{
    ESP_LOGI(TAG, "Firmware indirme basladi: %s", s_download_url);
    set_status(FW_OTA_DOWNLOADING, "Firmware indiriliyor...");
    set_progress(0);

    esp_http_client_config_t http_config = {
        .url = s_download_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_TIMEOUT_MS,
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin hatasi: %s", esp_err_to_name(err));
        set_status(FW_OTA_ERROR, "OTA baslatilamadi");
        goto done;
    }

    // Chunk chunk indir ve yaz
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        // Progress guncelle
        int total = esp_https_ota_get_image_size(handle);
        int read = esp_https_ota_get_image_len_read(handle);
        if (total > 0) {
            int pct = (read * 100) / total;
            set_progress(pct);
            if (pct % 10 == 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Indiriliyor... %%%d (%d/%d KB)",
                         pct, read / 1024, total / 1024);
                set_status(FW_OTA_DOWNLOADING, msg);
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform hatasi: %s", esp_err_to_name(err));
        set_status(FW_OTA_ERROR, "Firmware indirme hatasi");
        esp_https_ota_abort(handle);
        goto done;
    }

    // Dogrula ve tamamla
    err = esp_https_ota_finish(handle);
    handle = NULL;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Firmware guncelleme tamamlandi!");
        set_progress(100);
        set_status(FW_OTA_DONE, "Guncelleme tamamlandi! Yeniden baslatma gerekli.");
        update_rollback_info();
    } else if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGE(TAG, "Firmware dogrulama hatasi");
        set_status(FW_OTA_ERROR, "Firmware dogrulama basarisiz");
    } else {
        ESP_LOGE(TAG, "OTA finish hatasi: %s", esp_err_to_name(err));
        set_status(FW_OTA_ERROR, "Firmware yazma hatasi");
    }

done:
    if (handle) {
        esp_https_ota_abort(handle);
    }
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================================
// Public API
// ============================================================

esp_err_t fw_ota_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = FW_OTA_IDLE;
    s_download_url[0] = '\0';
    s_custom_url[0] = '\0';

    // Mevcut versiyon
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(s_status.current_version, app_desc->version,
            sizeof(s_status.current_version) - 1);

    // Rollback bilgisi
    update_rollback_info();

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "FW OTA baslatildi (partition: %s, v%s)",
             running->label, app_desc->version);

    if (s_rollback_available) {
        ESP_LOGI(TAG, "Rollback mevcut: v%s", s_rollback_version);
    }

    return ESP_OK;
}

esp_err_t fw_ota_check(const char *custom_url)
{
    if (s_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Custom URL'i sakla
    if (custom_url && strlen(custom_url) > 0) {
        strncpy(s_custom_url, custom_url, sizeof(s_custom_url) - 1);
        s_custom_url[sizeof(s_custom_url) - 1] = '\0';
    } else {
        s_custom_url[0] = '\0';
    }

    // Status sifirla
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = FW_OTA_CHECKING;
    strncpy(s_status.message, "Baslatiliyor...", sizeof(s_status.message) - 1);

    BaseType_t ret = xTaskCreate(fw_ota_check_task, "fw_check",
                                  OTA_TASK_STACK, NULL, 5, &s_task_handle);
    if (ret != pdPASS) {
        set_status(FW_OTA_ERROR, "Task olusturulamadi");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t fw_ota_start(void)
{
    if (s_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status.state != FW_OTA_UPDATE_AVAILABLE) {
        ESP_LOGW(TAG, "Start sadece UPDATE_AVAILABLE durumunda cagrilabilir");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_download_url[0] == '\0') {
        ESP_LOGE(TAG, "Indirme URL'i bos");
        return ESP_ERR_INVALID_ARG;
    }

    s_status.state = FW_OTA_DOWNLOADING;
    s_status.progress_pct = 0;
    strncpy(s_status.message, "Baslatiliyor...", sizeof(s_status.message) - 1);

    BaseType_t ret = xTaskCreate(fw_ota_download_task, "fw_ota",
                                  OTA_TASK_STACK, NULL, 5, &s_task_handle);
    if (ret != pdPASS) {
        set_status(FW_OTA_ERROR, "Task olusturulamadi");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void fw_ota_get_status(fw_ota_status_t *status)
{
    if (!status) return;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(status, &s_status, sizeof(fw_ota_status_t));
    if (s_mutex) xSemaphoreGive(s_mutex);
}

esp_err_t fw_ota_rollback(void)
{
    if (s_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (!other) {
        ESP_LOGW(TAG, "Rollback partition bulunamadi");
        return ESP_ERR_NOT_FOUND;
    }

    // Gecerli firmware var mi kontrol et
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(other, &desc) != ESP_OK) {
        ESP_LOGW(TAG, "Rollback partition'da gecerli firmware yok");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Boot partition degistirilemedi: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Rollback ayarlandi: %s (v%s) -> restart gerekli",
             other->label, desc.version);
    return ESP_OK;
}

bool fw_ota_is_busy(void)
{
    return (s_task_handle != NULL);
}

bool fw_ota_can_rollback(void)
{
    return s_rollback_available;
}

const char *fw_ota_get_running_label(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "unknown";
}

const char *fw_ota_get_rollback_version(void)
{
    if (s_rollback_available && s_rollback_version[0] != '\0') {
        return s_rollback_version;
    }
    return NULL;
}
