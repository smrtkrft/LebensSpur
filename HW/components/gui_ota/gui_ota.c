/**
 * LebensSpur - GUI OTA Guncelleme
 * GitHub'dan web arayuzu dosyalarini indirip inaktif slot'a yazar
 * A/B slot semasiyla guvenli guncelleme ve rollback destegi
 */

#include "gui_ota.h"
#include "ext_flash.h"
#include "device_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "GUI_OTA";

// Resmi kaynak
#define DEFAULT_REPO_URL  "https://raw.githubusercontent.com/smrtkrft/LebensSpur/main/GUI/"

// Indirme ayarlari
#define DOWNLOAD_CHUNK_SIZE  4096
#define HTTP_TIMEOUT_MS      15000

// Dosya listesi (12 dosya)
static const char *s_file_list[] = {
    "index.html",
    "style.css",
    "app.js",
    "config.json",
    "pic/logo.png",
    "pic/darklogo.png",
    "lang/en.json",
    "lang/de.json",
    "lang/es.json",
    "lang/fr.json",
    "lang/it.json",
    "lang/tr.json",
};
#define FILE_COUNT (sizeof(s_file_list) / sizeof(s_file_list[0]))

// Durum
static gui_ota_status_t s_status;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task_handle = NULL;

// Aktif OTA parametreleri
static gui_ota_params_t s_params;

// ============================================================
// Yardimci fonksiyonlar
// ============================================================

static void set_status(gui_ota_state_t state, const char *msg)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = state;
    if (msg) {
        strncpy(s_status.message, msg, sizeof(s_status.message) - 1);
        s_status.message[sizeof(s_status.message) - 1] = '\0';
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
}

static void set_progress(int file_index, int file_count, int pct)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.file_index = file_index;
    s_status.file_count = file_count;
    s_status.progress_pct = pct;
    if (s_mutex) xSemaphoreGive(s_mutex);
}

// Basit JSON string parser: "key":"value" formatinda
static int parse_json_value(const char *json, const char *key, char *out, size_t out_size)
{
    char search[64];
    // Bosluklu format: "key" : "value"
    snprintf(search, sizeof(search), "\"%s\"", key);
    char *key_pos = strstr(json, search);
    if (!key_pos) return -1;

    // key'den sonra : isareti bul
    char *colon = strchr(key_pos + strlen(search), ':');
    if (!colon) return -1;

    // " bul (bosluk atlayarak)
    char *quote_start = strchr(colon + 1, '"');
    if (!quote_start) return -1;
    quote_start++;

    char *quote_end = strchr(quote_start, '"');
    if (!quote_end) return -1;

    size_t len = quote_end - quote_start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, quote_start, len);
    out[len] = '\0';
    return (int)len;
}

// URL olustur: resmi veya custom kaynak
static void build_url(char *url, size_t url_size, const char *file)
{
    if (s_params.use_custom_source && strlen(s_params.repo_url) > 0) {
        // github.com -> raw.githubusercontent.com donusumu
        // https://github.com/user/repo -> https://raw.githubusercontent.com/user/repo/branch/GUI/
        char raw_url[200];
        if (strstr(s_params.repo_url, "raw.githubusercontent.com")) {
            // Zaten raw URL
            strncpy(raw_url, s_params.repo_url, sizeof(raw_url) - 1);
            raw_url[sizeof(raw_url) - 1] = '\0';
        } else if (strstr(s_params.repo_url, "github.com")) {
            // github.com -> raw.githubusercontent.com donusumu
            const char *path_start = strstr(s_params.repo_url, "github.com/");
            if (path_start) {
                path_start += strlen("github.com/");
                const char *branch = strlen(s_params.branch) > 0 ? s_params.branch : "main";
                snprintf(raw_url, sizeof(raw_url),
                         "https://raw.githubusercontent.com/%s/%s/GUI/",
                         path_start, branch);
            } else {
                strncpy(raw_url, s_params.repo_url, sizeof(raw_url) - 1);
                raw_url[sizeof(raw_url) - 1] = '\0';
            }
        } else {
            strncpy(raw_url, s_params.repo_url, sizeof(raw_url) - 1);
            raw_url[sizeof(raw_url) - 1] = '\0';
        }
        // Son slash kontrolu
        size_t len = strlen(raw_url);
        if (len > 0 && raw_url[len - 1] != '/') {
            if (len < sizeof(raw_url) - 1) {
                raw_url[len] = '/';
                raw_url[len + 1] = '\0';
            }
        }
        snprintf(url, url_size, "%s%s", raw_url, file);
    } else {
        snprintf(url, url_size, "%s%s", DEFAULT_REPO_URL, file);
    }
}

// Dizin ve icindeki tum dosyalari recursive sil
static void delete_dir_contents(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    char path[320];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                delete_dir_contents(path);
                rmdir(path);
            } else {
                remove(path);
            }
        }
    }
    closedir(dir);
}

// HTTP ile dosya indir ve belirtilen path'e yaz
static esp_err_t download_file_to_path(const char *url, const char *dest_path)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = DOWNLOAD_CHUNK_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open hatasi: %s -> %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP %d: %s", status_code, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Dosya olusturulamadi: %s", dest_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buf = malloc(DOWNLOAD_CHUNK_SIZE);
    if (!buf) {
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, buf, DOWNLOAD_CHUNK_SIZE)) > 0) {
        fwrite(buf, 1, read_len, f);
        total_read += read_len;
    }

    free(buf);
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGE(TAG, "Bos dosya indirildi: %s", url);
        remove(dest_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Indirildi: %s (%d byte)", dest_path, total_read);
    (void)content_length;
    return ESP_OK;
}

// Remote config.json indir ve versiyon parse et
static esp_err_t check_remote_version(char *version_out, size_t version_size)
{
    char url[256];
    build_url(url, sizeof(url), "config.json");

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    // Yaniti buffer'a oku
    char buf[512] = {0};
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "Remote config.json HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int total = 0;
    int len;
    while ((len = esp_http_client_read(client, buf + total, sizeof(buf) - total - 1)) > 0) {
        total += len;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // Version parse et
    if (parse_json_value(buf, "version", version_out, version_size) < 0) {
        ESP_LOGE(TAG, "Remote config.json'da version bulunamadi");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Remote versiyon: %s", version_out);
    return ESP_OK;
}

// Mevcut GUI versiyonunu oku (aktif slot'taki config.json'dan)
static void read_current_version(char *version_out, size_t version_size)
{
    version_out[0] = '\0';
    const char *mount = ext_flash_get_active_mount();
    char path[64];
    snprintf(path, sizeof(path), "%s/config.json", mount);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char buf[256];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    buf[len] = '\0';
    fclose(f);

    parse_json_value(buf, "version", version_out, version_size);
}

// ============================================================
// Ana OTA Task
// ============================================================

static void gui_ota_task(void *arg)
{
    ESP_LOGI(TAG, "GUI OTA task baslatildi");

    // 1. Mevcut versiyonu oku
    char current_version[16] = {0};
    read_current_version(current_version, sizeof(current_version));
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_status.current_version, current_version, sizeof(s_status.current_version) - 1);
    if (s_mutex) xSemaphoreGive(s_mutex);

    // 2. Remote versiyon kontrol
    set_status(GUI_OTA_CHECKING, "Versiyon kontrol ediliyor...");
    char remote_version[16] = {0};
    if (check_remote_version(remote_version, sizeof(remote_version)) != ESP_OK) {
        set_status(GUI_OTA_ERROR, "Uzak sunucuya baglanilamadi");
        goto done;
    }

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_status.remote_version, remote_version, sizeof(s_status.remote_version) - 1);
    if (s_mutex) xSemaphoreGive(s_mutex);

    // 3. Versiyon karsilastir
    if (strlen(current_version) > 0 && strcmp(current_version, remote_version) == 0) {
        ESP_LOGI(TAG, "GUI zaten guncel: v%s", current_version);
        set_status(GUI_OTA_NO_UPDATE, "Zaten guncel");
        goto done;
    }

    ESP_LOGI(TAG, "Guncelleme mevcut: v%s -> v%s", current_version, remote_version);
    device_log_add(LOG_TYPE_OTA, "GUI OTA baslatildi: v%s -> v%s", current_version, remote_version);

    // 4. Inaktif slot'u belirle ve temizle
    set_status(GUI_OTA_DOWNLOADING, "Hedef slot hazirlaniyor...");
    const char *target_mount = ext_flash_get_inactive_mount();
    ESP_LOGI(TAG, "Hedef slot: %s", target_mount);

    delete_dir_contents(target_mount);

    // 5. Alt dizinleri olustur
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/pic", target_mount);
    mkdir(dir_path, 0755);
    snprintf(dir_path, sizeof(dir_path), "%s/lang", target_mount);
    mkdir(dir_path, 0755);

    // 6. Dosyalari indir
    int file_count = (int)FILE_COUNT;
    for (int i = 0; i < file_count; i++) {
        char url[256];
        build_url(url, sizeof(url), s_file_list[i]);

        char dest[128];
        snprintf(dest, sizeof(dest), "%s/%s", target_mount, s_file_list[i]);

        int pct = ((i + 1) * 100) / file_count;
        set_progress(i + 1, file_count, pct);

        char msg[128];
        snprintf(msg, sizeof(msg), "%s indiriliyor... (%d/%d)", s_file_list[i], i + 1, file_count);
        set_status(GUI_OTA_DOWNLOADING, msg);

        esp_err_t err = download_file_to_path(url, dest);
        if (err != ESP_OK) {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg), "%s indirilemedi", s_file_list[i]);
            device_log_add(LOG_TYPE_OTA, "GUI OTA hata: %s", err_msg);
            set_status(GUI_OTA_ERROR, err_msg);
            goto done;
        }

        // Task'i bloke etmeden kucuk gecikme
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 7. Kritik dosyalari dogrula
    set_status(GUI_OTA_VERIFYING, "Dogrulaniyor...");
    {
        char check_path[128];
        snprintf(check_path, sizeof(check_path), "%s/config.json", target_mount);
        struct stat st;
        if (stat(check_path, &st) != 0 || st.st_size < 10) {
            set_status(GUI_OTA_ERROR, "config.json dogrulanamadi");
            goto done;
        }
        snprintf(check_path, sizeof(check_path), "%s/index.html", target_mount);
        if (stat(check_path, &st) != 0 || st.st_size < 10) {
            set_status(GUI_OTA_ERROR, "index.html dogrulanamadi");
            goto done;
        }
    }

    // 8. Aktif slot'u degistir
    set_status(GUI_OTA_SWITCHING, "Slot degistiriliyor...");
    char new_slot = (ext_flash_get_active_slot() == 'a') ? 'b' : 'a';
    esp_err_t slot_err = ext_flash_set_active_slot(new_slot);
    if (slot_err != ESP_OK) {
        set_status(GUI_OTA_ERROR, "Slot degistirilemedi");
        goto done;
    }

    ESP_LOGI(TAG, "GUI OTA tamamlandi! Yeni aktif slot: %c, v%s", new_slot, remote_version);
    device_log_add(LOG_TYPE_OTA, "GUI OTA tamamlandi: v%s (slot %c)", remote_version, new_slot);
    set_status(GUI_OTA_DONE, "Guncelleme tamamlandi!");
    set_progress(file_count, file_count, 100);

done:
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================================
// Public API
// ============================================================

esp_err_t gui_ota_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = GUI_OTA_IDLE;
    s_status.file_count = FILE_COUNT;

    ESP_LOGI(TAG, "GUI OTA baslatildi");
    return ESP_OK;
}

esp_err_t gui_ota_start(const gui_ota_params_t *params)
{
    if (s_task_handle != NULL) {
        ESP_LOGW(TAG, "OTA zaten calisiyor");
        return ESP_ERR_INVALID_STATE;
    }

    // Parametreleri kopyala
    if (params) {
        memcpy(&s_params, params, sizeof(s_params));
    } else {
        memset(&s_params, 0, sizeof(s_params));
    }

    // Status sifirla
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = GUI_OTA_CHECKING;
    s_status.file_count = FILE_COUNT;
    strncpy(s_status.message, "Baslatiliyor...", sizeof(s_status.message) - 1);

    BaseType_t ret = xTaskCreate(gui_ota_task, "gui_ota", 10240, NULL, 5, &s_task_handle);
    if (ret != pdPASS) {
        set_status(GUI_OTA_ERROR, "Task olusturulamadi");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void gui_ota_get_status(gui_ota_status_t *status)
{
    if (!status) return;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(status, &s_status, sizeof(gui_ota_status_t));
    if (s_mutex) xSemaphoreGive(s_mutex);
}

esp_err_t gui_ota_rollback(void)
{
    if (s_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char current = ext_flash_get_active_slot();
    char target = (current == 'a') ? 'b' : 'a';

    // Hedef slot'ta config.json var mi kontrol et
    const char *target_mount = (target == 'a') ? "/slot_a" : "/slot_b";
    char path[64];
    snprintf(path, sizeof(path), "%s/config.json", target_mount);
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "Rollback hedef slot'ta config.json yok: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ext_flash_set_active_slot(target);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Rollback basarili: slot %c -> %c", current, target);
        device_log_add(LOG_TYPE_OTA, "GUI rollback: slot %c -> %c", current, target);
    }
    return err;
}

bool gui_ota_is_busy(void)
{
    return (s_task_handle != NULL);
}
