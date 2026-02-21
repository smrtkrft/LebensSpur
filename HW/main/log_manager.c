/**
 * Log Manager - LittleFS Log Yazma
 *
 * /ext/logs/ dizininde timestamp tabanlı log dosyaları.
 * Dosya boyutu aşıldığında otomatik rotasyon.
 * Her yazma append modda yapılır.
 */

#include "log_manager.h"
#include "file_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "LOG_MGR";

static bool s_initialized = false;
static log_level_t s_level = LOG_LEVEL_INFO;
static char s_current_file[128] = {0};
static uint32_t s_current_size = 0;
static uint32_t s_log_count = 0;

static const char *level_str(log_level_t level)
{
    switch (level) {
        case LOG_LEVEL_ERROR:   return "E";
        case LOG_LEVEL_WARN:    return "W";
        case LOG_LEVEL_INFO:    return "I";
        case LOG_LEVEL_DEBUG:   return "D";
        case LOG_LEVEL_VERBOSE: return "V";
        default:                return "?";
    }
}

static esp_err_t create_new_file(void)
{
    int64_t ts = esp_timer_get_time() / 1000000;
    snprintf(s_current_file, sizeof(s_current_file),
             FILE_MGR_LOG_PATH "/log_%lld.txt", ts);

    esp_err_t ret = file_manager_write_string(s_current_file, "");
    if (ret == ESP_OK) {
        s_current_size = 0;
        ESP_LOGI(TAG, "Yeni log dosyası: %s", s_current_file);
    }
    return ret;
}

static void cleanup_old(void)
{
    DIR *dir = opendir(FILE_MGR_LOG_PATH);
    if (!dir) return;

    char oldest[280] = {0};
    int64_t oldest_ts = INT64_MAX;
    int count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "log_", 4) != 0) continue;
        count++;

        int64_t ts = 0;
        if (sscanf(entry->d_name, "log_%lld.txt", &ts) == 1 && ts < oldest_ts) {
            oldest_ts = ts;
            snprintf(oldest, sizeof(oldest), FILE_MGR_LOG_PATH "/%s", entry->d_name);
        }
    }
    closedir(dir);

    if (count >= LOG_MGR_MAX_FILES && oldest[0]) {
        file_manager_delete(oldest);
        ESP_LOGI(TAG, "Eski log silindi: %s", oldest);
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t log_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    // Log dizininin var olduğundan emin ol
    file_manager_mkdir(FILE_MGR_LOG_PATH);

    esp_err_t ret = create_new_file();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Log dosyası oluşturulamadı");
        return ret;
    }

    s_initialized = true;
    log_manager_write(LOG_LEVEL_INFO, TAG, "Log sistemi baslatildi");

    ESP_LOGI(TAG, "OK - %s", FILE_MGR_LOG_PATH);
    return ESP_OK;
}

esp_err_t log_manager_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    s_initialized = false;
    return ESP_OK;
}

void log_manager_set_level(log_level_t level)
{
    s_level = level;
}

void log_manager_write(log_level_t level, const char *tag, const char *format, ...)
{
    if (!s_initialized || level > s_level) return;

    // Mesajı formatla
    char msg[LOG_MGR_BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    // Timestamp (ms since boot)
    int64_t ms = esp_timer_get_time() / 1000;

    // Log satırı
    char line[LOG_MGR_BUFFER_SIZE + 64];
    int len = snprintf(line, sizeof(line), "[%lld][%s][%s] %s\n",
                       ms, level_str(level), tag, msg);

    // Dosya boyutu kontrolü - rotasyon
    if (s_current_size + len > LOG_MGR_MAX_FILE_SIZE) {
        cleanup_old();
        create_new_file();
    }

    // Dosyaya append
    if (file_manager_append(s_current_file, line, len) == ESP_OK) {
        s_current_size += len;
        s_log_count++;
    }
}

const char *log_manager_get_current_file(void)
{
    return s_current_file;
}

esp_err_t log_manager_list_files(char files[][64], size_t max_files, size_t *count)
{
    DIR *dir = opendir(FILE_MGR_LOG_PATH);
    if (!dir) {
        if (count) *count = 0;
        return ESP_FAIL;
    }

    size_t idx = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && idx < max_files) {
        if (strncmp(entry->d_name, "log_", 4) == 0) {
            strncpy(files[idx], entry->d_name, 63);
            files[idx][63] = '\0';
            idx++;
        }
    }
    closedir(dir);

    if (count) *count = idx;
    return ESP_OK;
}

esp_err_t log_manager_read_file(const char *filename, char *buffer, size_t max_size, size_t *read_size)
{
    char path[128];
    snprintf(path, sizeof(path), FILE_MGR_LOG_PATH "/%s", filename);
    return file_manager_read(path, buffer, max_size, read_size);
}

esp_err_t log_manager_clear_all(void)
{
    ESP_LOGW(TAG, "Tüm loglar siliniyor...");

    DIR *dir = opendir(FILE_MGR_LOG_PATH);
    if (!dir) return ESP_FAIL;

    struct dirent *entry;
    char path[280];
    int deleted = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "log_", 4) == 0) {
            snprintf(path, sizeof(path), FILE_MGR_LOG_PATH "/%s", entry->d_name);
            if (file_manager_delete(path) == ESP_OK) {
                deleted++;
            }
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "%d log dosyası silindi", deleted);
    return create_new_file();
}

esp_err_t log_manager_rotate(void)
{
    cleanup_old();
    return create_new_file();
}

esp_err_t log_manager_get_stats(uint32_t *total_size, uint32_t *file_count)
{
    DIR *dir = opendir(FILE_MGR_LOG_PATH);
    if (!dir) {
        if (total_size) *total_size = 0;
        if (file_count) *file_count = 0;
        return ESP_FAIL;
    }

    uint32_t size = 0, count = 0;
    struct dirent *entry;
    struct stat st;
    char path[280];

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "log_", 4) == 0) {
            snprintf(path, sizeof(path), FILE_MGR_LOG_PATH "/%s", entry->d_name);
            if (stat(path, &st) == 0) {
                size += st.st_size;
            }
            count++;
        }
    }
    closedir(dir);

    if (total_size) *total_size = size;
    if (file_count) *file_count = count;
    return ESP_OK;
}

esp_err_t log_manager_flush(void)
{
    // LittleFS'te her write fclose ile tamamlanır, explicit flush gerekmez
    return ESP_OK;
}

void log_manager_print_info(void)
{
    ESP_LOGI(TAG, "┌──────────────────────────────────────");

    if (!s_initialized) {
        ESP_LOGW(TAG, "│ Durum:     BAŞLATILMAMIŞ");
        ESP_LOGI(TAG, "└──────────────────────────────────────");
        return;
    }

    uint32_t total_size = 0, file_count = 0;
    log_manager_get_stats(&total_size, &file_count);

    ESP_LOGI(TAG, "│ Durum:     AKTİF");
    ESP_LOGI(TAG, "│ Seviye:    %d", s_level);
    ESP_LOGI(TAG, "│ Dosya:     %s", s_current_file);
    ESP_LOGI(TAG, "│ Dosya #:   %lu", file_count);
    ESP_LOGI(TAG, "│ Toplam:    %lu KB", total_size / 1024);
    ESP_LOGI(TAG, "│ Log #:     %lu", s_log_count);
    ESP_LOGI(TAG, "│ Max dosya: %d KB", LOG_MGR_MAX_FILE_SIZE / 1024);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
