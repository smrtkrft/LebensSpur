/**
 * GUI Downloader - GitHub'dan Web Arayuzu Indirme
 *
 * GitHub raw URL uzerinden GUI dosyalarini indirir.
 * Fastly CDN IP adresleri ile DNS bypass destegi.
 * APSTA modunda AP gecici olarak kapatilir (DNS routing sorunu).
 */

#include "gui_downloader.h"
#include "file_manager.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "GUI_DL";

// Varsayilan repo bilgileri
#define DEFAULT_REPO    "smrtkrft/LebensSpur"
#define DEFAULT_BRANCH  "main"
#define DEFAULT_PATH    "GUI"

// GitHub raw URL
#define GITHUB_RAW_HOST "raw.githubusercontent.com"

// GitHub CDN IP adresleri (Fastly) - DNS bypass
static const char *GITHUB_IPS[] = {
    "185.199.108.133",
    "185.199.109.133",
    "185.199.110.133",
    "185.199.111.133"
};
#define GITHUB_IP_COUNT 4

// Indirilecek dosyalar: GitHub kaynak yolu -> yerel kayit adi
typedef struct {
    const char *github;     // GitHub repo icindeki yol (orn: "js/app.js")
    const char *local;      // Yerel kayit adi (orn: "app.js")
} gui_file_entry_t;

static const gui_file_entry_t GUI_FILES[] = {
    { "index.html",             "index.html" },
    { "style.css",              "style.css" },
    { "js/state.js",            "state.js" },
    { "js/utils.js",            "utils.js" },
    { "js/ui.js",               "ui.js" },
    { "js/auth.js",             "auth.js" },
    { "js/timer.js",            "timer.js" },
    { "js/settings.js",         "settings.js" },
    { "js/actions.js",          "actions.js" },
    { "js/mailGroups.js",       "mailGroups.js" },
    { "js/logs.js",             "logs.js" },
    { "js/ota.js",              "ota.js" },
    { "js/theme.js",            "theme.js" },
    { "js/app.js",              "app.js" },
    { "js/i18n.js",             "i18n.js" },
    { "manifest.json",          "manifest.json" },
    { "sw.js",                  "sw.js" },
    { "pic/logo.png",           "logo.png" },
    { "pic/darklogo.png",       "darklogo.png" },
    { "i18n/en.json",           "i18n/en.json" },
    { "i18n/tr.json",           "i18n/tr.json" },
    { NULL, NULL }
};

// Kritik dosyalar - yerel isimler (bunlar olmadan GUI calismaz)
static const char *CRITICAL_FILES[] = {
    "index.html",
    "style.css",
    "state.js",
    "utils.js",
    "ui.js",
    "app.js",
    NULL
};

// Yerel isimle kritik mi kontrolu
static bool is_critical(const char *local_name)
{
    for (int i = 0; CRITICAL_FILES[i]; i++) {
        if (strcmp(CRITICAL_FILES[i], local_name) == 0) return true;
    }
    return false;
}

// HTTP buffer boyutu
#define HTTP_BUFFER_SIZE    (32 * 1024)
#define MAX_RETRIES         4
#define RETRY_DELAY_MS      2000
#define WIFI_WAIT_TICKS     60      // 30 saniye (500ms araliklarda)

// HTTP indirme context'i (binary-safe offset takibi)
typedef struct {
    char *buffer;
    int offset;
} http_dl_ctx_t;

// Durum degiskenleri
static gui_dl_status_t s_status;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_cancel = false;

// Indirme parametreleri
static char s_repo[64];
static char s_branch[32];
static char s_path[32];

// ============================================================================
// Dahili yardimcilar
// ============================================================================

static void status_set(gui_dl_state_t state, uint8_t progress, const char *msg)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.state = state;
        s_status.progress = progress;
        if (msg) {
            strncpy(s_status.message, msg, sizeof(s_status.message) - 1);
            s_status.message[sizeof(s_status.message) - 1] = '\0';
        }
        xSemaphoreGive(s_mutex);
    }
}

static void status_error(const char *err)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.state = GUI_DL_STATE_ERROR;
        if (err) {
            strncpy(s_status.error, err, sizeof(s_status.error) - 1);
            s_status.error[sizeof(s_status.error) - 1] = '\0';
        }
        xSemaphoreGive(s_mutex);
    }
}

static void status_update_bytes(uint32_t added, uint8_t file_idx)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.bytes_downloaded += added;
        s_status.files_downloaded = file_idx + 1;
        xSemaphoreGive(s_mutex);
    }
}

static int count_files(void)
{
    int n = 0;
    while (GUI_FILES[n].github) n++;
    return n;
}

// HTTP event handler - buffer'a veri yazar (binary-safe)
// user_data = http_dl_ctx_t* (buffer + offset)
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                http_dl_ctx_t *ctx = (http_dl_ctx_t *)evt->user_data;
                if (ctx && ctx->buffer) {
                    if (ctx->offset + evt->data_len < HTTP_BUFFER_SIZE) {
                        memcpy(ctx->buffer + ctx->offset, evt->data, evt->data_len);
                        ctx->offset += evt->data_len;
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Tek dosya indir (retry destekli, CDN IP rotasyonu)
// github_name: GitHub repo icindeki yol (orn: "js/app.js")
// local_name:  Yerel kayit adi (orn: "app.js")
static esp_err_t download_file(const char *github_name, const char *local_name,
                               int file_idx, int total, char *buffer)
{
    // URL path olustur (GitHub kaynak yolunu kullan)
    char url_path[256];
    snprintf(url_path, sizeof(url_path), "/%s/%s/%s/%s",
             s_repo, s_branch, s_path, github_name);

    // Yerel dosya yolu (duzlestirilmis yerel ismi kullan)
    char local_path[128];
    snprintf(local_path, sizeof(local_path), "%s/%s", FILE_MGR_WEB_PATH, local_name);

    // Alt klasor varsa olustur (ornegin "i18n/en.json")
    const char *slash = strrchr(local_name, '/');
    if (slash) {
        char dir[128];
        int dir_len = (int)(slash - local_name);
        snprintf(dir, sizeof(dir), "%s/%.*s", FILE_MGR_WEB_PATH, dir_len, local_name);
        file_manager_mkdir(dir);
    }

    ESP_LOGI(TAG, "Indiriliyor: %s -> %s", github_name, local_name);

    esp_err_t err = ESP_FAIL;

    for (int retry = 0; retry < MAX_RETRIES && err != ESP_OK; retry++) {
        if (s_cancel) return ESP_ERR_INVALID_STATE;

        if (retry > 0) {
            ESP_LOGI(TAG, "Tekrar %d: %s", retry, github_name);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }

        const char *ip = GITHUB_IPS[retry % GITHUB_IP_COUNT];

        // Buffer ve context'i temizle
        memset(buffer, 0, HTTP_BUFFER_SIZE);
        http_dl_ctx_t ctx = { .buffer = buffer, .offset = 0 };

        esp_http_client_config_t cfg = {
            .host = ip,
            .port = 443,
            .path = url_path,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .event_handler = http_event_handler,
            .user_data = &ctx,
            .timeout_ms = 30000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .skip_cert_common_name_check = true,    // IP kullandigimiz icin
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "HTTP client init hatasi");
            continue;
        }

        // Host header (SNI icin gerekli)
        esp_http_client_set_header(client, "Host", GITHUB_RAW_HOST);

        err = esp_http_client_perform(client);

        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            int content_len = esp_http_client_get_content_length(client);

            if (status == 200 && content_len > 0) {
                err = file_manager_write(local_path, buffer, content_len);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Kaydedildi: %s (%d byte)", local_path, content_len);
                    status_update_bytes(content_len, file_idx);
                } else {
                    ESP_LOGE(TAG, "Dosya yazma hatasi: %s", local_path);
                }
            } else {
                ESP_LOGE(TAG, "HTTP hata: status=%d, len=%d", status, content_len);
                err = ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "HTTP istek hatasi: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }

    return err;
}

// ============================================================================
// Indirme task
// ============================================================================

static void download_task(void *arg)
{
    int total = count_files();
    int success = 0;

    // ONEMLI: original_mode goto'dan ONCE tanimlanmali (scope hatasi fix)
    wifi_mode_t original_mode = WIFI_MODE_NULL;

    ESP_LOGI(TAG, "GUI indirme basliyor: %s/%s/%s", s_repo, s_branch, s_path);

    // WiFi STA baglantisinini bekle (max 30 saniye)
    status_set(GUI_DL_STATE_CONNECTING, 0, "WiFi bekleniyor...");
    int wait = 0;
    while (!wifi_manager_is_connected() && wait < WIFI_WAIT_TICKS && !s_cancel) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait++;
        status_set(GUI_DL_STATE_CONNECTING, (wait * 5) / WIFI_WAIT_TICKS, "WiFi bekleniyor...");
    }

    if (!wifi_manager_is_connected()) {
        ESP_LOGE(TAG, "WiFi baglantisi yok");
        status_error("WiFi baglantisi basarisiz");
        goto cleanup;
    }

    ESP_LOGI(TAG, "WiFi bagli, IP: %s", wifi_manager_get_ip());

    // APSTA modunda AP gecici olarak kapatilir (DNS routing sorunu)
    esp_wifi_get_mode(&original_mode);
    if (original_mode == WIFI_MODE_APSTA) {
        ESP_LOGI(TAG, "AP gecici olarak kapatiliyor...");
        esp_wifi_set_mode(WIFI_MODE_STA);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // DNS propagasyonu icin bekle
    status_set(GUI_DL_STATE_CONNECTING, 5, "DNS ayarlaniyor...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // HTTP buffer ayir
    char *buffer = malloc(HTTP_BUFFER_SIZE);
    if (!buffer) {
        status_error("Bellek yetersiz");
        goto cleanup;
    }

    // Web dizinlerini olustur
    status_set(GUI_DL_STATE_CONNECTING, 7, "Dizinler olusturuluyor...");
    file_manager_mkdir(FILE_MGR_WEB_PATH);
    file_manager_mkdir(FILE_MGR_WEB_PATH "/i18n");

    // Toplam dosya sayisini ayarla
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.total_files = total;
        s_status.files_downloaded = 0;
        s_status.bytes_downloaded = 0;
        xSemaphoreGive(s_mutex);
    }

    status_set(GUI_DL_STATE_DOWNLOADING, 10, "Indiriliyor...");

    // Her dosyayi indir
    for (int i = 0; GUI_FILES[i].github && !s_cancel; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Indiriliyor (%d/%d)...", i + 1, total);
        int progress = 10 + ((i * 80) / total);
        status_set(GUI_DL_STATE_DOWNLOADING, progress, msg);

        esp_err_t err = download_file(GUI_FILES[i].github, GUI_FILES[i].local,
                                      i, total, buffer);
        if (err == ESP_OK) {
            success++;
        } else {
            ESP_LOGW(TAG, "Indirme basarisiz: %s", GUI_FILES[i].github);
            if (is_critical(GUI_FILES[i].local)) {
                status_error("Kritik GUI dosyasi indirilemedi");
                free(buffer);
                goto cleanup;
            }
        }

        // Rate limiting
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(buffer);

    if (s_cancel) {
        status_error("Indirme iptal edildi");
        goto cleanup;
    }

    // Version dosyasi olustur
    status_set(GUI_DL_STATE_INSTALLING, 95, "Tamamlaniyor...");
    file_manager_write_string(FILE_MGR_WEB_PATH "/version.txt", "github-latest");

    ESP_LOGI(TAG, "GUI indirme tamamlandi: %d/%d dosya", success, total);
    status_set(GUI_DL_STATE_COMPLETE, 100, "Tamamlandi");

cleanup:
    // AP'yi geri ac (eger kapatildiysa)
    if (original_mode == WIFI_MODE_APSTA) {
        ESP_LOGI(TAG, "AP tekrar aciliyor...");
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t gui_downloader_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "Mutex olusturulamadi");
            return ESP_FAIL;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = GUI_DL_STATE_IDLE;

    ESP_LOGI(TAG, "OK");
    return ESP_OK;
}

esp_err_t gui_downloader_start(const char *repo, const char *branch, const char *path)
{
    if (s_task) {
        ESP_LOGW(TAG, "Indirme zaten devam ediyor");
        return ESP_ERR_INVALID_STATE;
    }

    // Parametreleri kaydet
    strncpy(s_repo, repo ? repo : DEFAULT_REPO, sizeof(s_repo) - 1);
    s_repo[sizeof(s_repo) - 1] = '\0';
    strncpy(s_branch, branch ? branch : DEFAULT_BRANCH, sizeof(s_branch) - 1);
    s_branch[sizeof(s_branch) - 1] = '\0';
    strncpy(s_path, path ? path : DEFAULT_PATH, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';

    // Durumu sifirla
    s_cancel = false;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = GUI_DL_STATE_CONNECTING;
    strncpy(s_status.message, "Baslatiliyor...", sizeof(s_status.message) - 1);

    BaseType_t ret = xTaskCreate(download_task, "gui_dl", 8192, NULL, 5, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Task olusturulamadi");
        s_status.state = GUI_DL_STATE_ERROR;
        strncpy(s_status.error, "Task olusturulamadi", sizeof(s_status.error) - 1);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void gui_downloader_get_status(gui_dl_status_t *status)
{
    if (!status) return;

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(status, &s_status, sizeof(gui_dl_status_t));
        xSemaphoreGive(s_mutex);
    } else {
        memset(status, 0, sizeof(gui_dl_status_t));
        status->state = GUI_DL_STATE_IDLE;
    }
}

void gui_downloader_cancel(void)
{
    s_cancel = true;
    ESP_LOGI(TAG, "Iptal istegi alindi");
}

bool gui_downloader_files_exist(void)
{
    return file_manager_exists(FILE_MGR_WEB_PATH "/index.html") &&
           file_manager_exists(FILE_MGR_WEB_PATH "/state.js") &&
           file_manager_exists(FILE_MGR_WEB_PATH "/app.js") &&
           file_manager_exists(FILE_MGR_WEB_PATH "/style.css");
}

const char *gui_downloader_get_default_repo(void)
{
    return DEFAULT_REPO;
}
