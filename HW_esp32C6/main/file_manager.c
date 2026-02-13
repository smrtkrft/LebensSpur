/**
 * File Manager - LittleFS Implementasyonu (Harici Flash)
 *
 * ext_flash üzerinde LittleFS partition kayıt edip mount eder.
 * LittleFS: gerçek dizin desteği, wear leveling, power-loss safe.
 * joltwallet/littlefs ESP-IDF component kullanır.
 */

#include "file_manager.h"
#include "ext_flash.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

static const char *TAG = "FILE_MGR";

static bool s_mounted = false;

#define PARTITION_LABEL "ext_storage"

// ============================================================================
// Init / Deinit
// ============================================================================

esp_err_t file_manager_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    if (!ext_flash_is_ready()) {
        ESP_LOGE(TAG, "Harici flash hazır değil!");
        return ESP_ERR_INVALID_STATE;
    }

    esp_flash_t *flash = ext_flash_get_handle();
    if (!flash) {
        ESP_LOGE(TAG, "Flash handle alınamadı");
        return ESP_ERR_INVALID_STATE;
    }

    // Harici flash üzerinde partition kaydet
    uint32_t flash_size = ext_flash_get_size();
    uint32_t part_size = (flash_size > FILE_MGR_PARTITION_SIZE)
                         ? FILE_MGR_PARTITION_SIZE : flash_size;

    const esp_partition_t *partition = NULL;
    esp_err_t ret = esp_partition_register_external(
        flash, 0, part_size,
        PARTITION_LABEL,
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        &partition
    );

    if (ret != ESP_OK || !partition) {
        ESP_LOGE(TAG, "Partition kayıt başarısız: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Partition: %s, %lu bytes (%lu MB)",
             partition->label, partition->size, partition->size / (1024 * 1024));

    // LittleFS mount
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FILE_MGR_BASE_PATH,
        .partition_label = PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount başarısız: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;

    // Alt dizinleri oluştur (LittleFS gerçek dizin destekler)
    file_manager_mkdir(FILE_MGR_WEB_PATH);
    file_manager_mkdir(FILE_MGR_LOG_PATH);
    file_manager_mkdir(FILE_MGR_CONFIG_PATH);
    file_manager_mkdir(FILE_MGR_DATA_PATH);

    ESP_LOGI(TAG, "LittleFS OK - mount: %s (%lu MB)",
             FILE_MGR_BASE_PATH, part_size / (1024 * 1024));

    return ESP_OK;
}

esp_err_t file_manager_deinit(void)
{
    if (!s_mounted) return ESP_OK;

    esp_err_t ret = esp_vfs_littlefs_unregister(PARTITION_LABEL);
    if (ret == ESP_OK) {
        s_mounted = false;
    }
    return ret;
}

// ============================================================================
// Yazma İşlemleri
// ============================================================================

esp_err_t file_manager_write(const char *path, const void *data, size_t size)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Açılamadı (yaz): %s errno=%d", path, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    if (written != size) {
        ESP_LOGE(TAG, "Yazma hatası: %s (%u/%u)", path, written, size);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t file_manager_write_string(const char *path, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    return file_manager_write(path, str, strlen(str));
}

esp_err_t file_manager_append(const char *path, const void *data, size_t size)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(path, "ab");
    if (!f) {
        ESP_LOGE(TAG, "Açılamadı (append): %s errno=%d", path, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    return (written == size) ? ESP_OK : ESP_FAIL;
}

// ============================================================================
// Okuma İşlemleri
// ============================================================================

esp_err_t file_manager_read(const char *path, void *buffer, size_t size, size_t *read_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Açılamadı (oku): %s errno=%d", path, errno);
        return ESP_FAIL;
    }

    size_t rd = fread(buffer, 1, size, f);
    fclose(f);

    if (read_bytes) {
        *read_bytes = rd;
    }

    return ESP_OK;
}

esp_err_t file_manager_read_string(const char *path, char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) return ESP_ERR_INVALID_ARG;

    size_t rd = 0;
    esp_err_t ret = file_manager_read(path, buffer, max_len - 1, &rd);
    if (ret == ESP_OK) {
        buffer[rd] = '\0';
    }
    return ret;
}

// ============================================================================
// Dosya İşlemleri
// ============================================================================

bool file_manager_exists(const char *path)
{
    if (!s_mounted) return false;

    struct stat st;
    return (stat(path, &st) == 0);
}

esp_err_t file_manager_delete(const char *path)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    if (remove(path) != 0) {
        ESP_LOGE(TAG, "Silinemedi: %s errno=%d", path, errno);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Silindi: %s", path);
    return ESP_OK;
}

int32_t file_manager_get_size(const char *path)
{
    if (!s_mounted) return -1;

    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (int32_t)st.st_size;
}

esp_err_t file_manager_mkdir(const char *path)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;  // Zaten var
    }

    if (mkdir(path, 0775) != 0) {
        if (errno == EEXIST) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Dizin oluşturulamadı: %s errno=%d", path, errno);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Dizin oluşturuldu: %s", path);
    return ESP_OK;
}

esp_err_t file_manager_list_dir(const char *path, file_info_t *files, size_t max_files, size_t *count)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Dizin açılamadı: %s errno=%d", path, errno);
        if (count) *count = 0;
        return ESP_FAIL;
    }

    size_t idx = 0;
    struct dirent *entry;
    struct stat st;
    char full_path[FILE_MGR_MAX_PATH_LEN];

    while ((entry = readdir(dir)) != NULL && idx < max_files) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        strncpy(files[idx].name, entry->d_name, sizeof(files[idx].name) - 1);
        files[idx].name[sizeof(files[idx].name) - 1] = '\0';

        if (stat(full_path, &st) == 0) {
            files[idx].size = (uint32_t)st.st_size;
            files[idx].is_dir = S_ISDIR(st.st_mode);
        } else {
            files[idx].size = 0;
            files[idx].is_dir = false;
        }

        idx++;
    }

    closedir(dir);

    if (count) *count = idx;
    return ESP_OK;
}

// ============================================================================
// Bilgi & Yardımcı
// ============================================================================

esp_err_t file_manager_get_info(uint32_t *total_bytes, uint32_t *used_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    size_t total = 0, used = 0;
    esp_err_t ret = esp_littlefs_info(PARTITION_LABEL, &total, &used);

    if (ret == ESP_OK) {
        if (total_bytes) *total_bytes = (uint32_t)total;
        if (used_bytes) *used_bytes = (uint32_t)used;
    }

    return ret;
}

bool file_manager_is_mounted(void)
{
    return s_mounted;
}

esp_err_t file_manager_format(void)
{
    ESP_LOGW(TAG, "LittleFS formatlanıyor...");
    return esp_littlefs_format(PARTITION_LABEL);
}

FILE *file_manager_fopen(const char *path, const char *mode)
{
    if (!s_mounted) return NULL;
    return fopen(path, mode);
}

void file_manager_print_info(void)
{
    ESP_LOGI(TAG, "┌──────────────────────────────────────");

    if (!s_mounted) {
        ESP_LOGW(TAG, "│ Durum:     MOUNT EDİLMEMİŞ");
        ESP_LOGI(TAG, "└──────────────────────────────────────");
        return;
    }

    uint32_t total = 0, used = 0;
    if (file_manager_get_info(&total, &used) == ESP_OK) {
        uint32_t free_b = total - used;
        ESP_LOGI(TAG, "│ Durum:     LittleFS HAZIR");
        ESP_LOGI(TAG, "│ Mount:     %s", FILE_MGR_BASE_PATH);
        ESP_LOGI(TAG, "│ Toplam:    %lu KB (%lu MB)", total / 1024, total / (1024 * 1024));
        ESP_LOGI(TAG, "│ Kullanım:  %lu KB (%.1f%%)", used / 1024,
                 total > 0 ? (used * 100.0f / total) : 0);
        ESP_LOGI(TAG, "│ Boş:       %lu KB (%lu MB)", free_b / 1024, free_b / (1024 * 1024));
    }

    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
