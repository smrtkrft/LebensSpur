/**
 * @file file_manager.c
 * @brief File System Manager - SPIFFS Implementation
 */

#include "file_manager.h"
#include "ext_flash.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

static const char *TAG = "file_mgr";

static bool s_mounted = false;

#define EXT_FLASH_PARTITION_LABEL "ext_storage"

esp_err_t file_manager_init(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "Already mounted");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing SPIFFS on external flash...");
    
    if (!ext_flash_is_ready()) {
        ESP_LOGE(TAG, "External flash not ready!");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_flash_t *flash = ext_flash_get_handle();
    if (!flash) {
        ESP_LOGE(TAG, "Could not get flash handle");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Register external flash as partition
    // SPIFFS page_ix is uint16_t (max 65535 pages), with 256B page = ~16.7MB max
    // To be safe, limit to 15MB (15*1024*1024 / 256 = 61440 pages < 65535)
    const esp_partition_t *partition = NULL;
    uint32_t partition_size = ext_flash_get_size();
    const uint32_t MAX_SPIFFS_SIZE = 15 * 1024 * 1024; // 15MB max for SPIFFS
    if (partition_size > MAX_SPIFFS_SIZE) {
        partition_size = MAX_SPIFFS_SIZE;
        ESP_LOGW(TAG, "Limiting partition to 15MB for SPIFFS page_ix compatibility");
    }
    esp_err_t err = esp_partition_register_external(
        flash,
        0,                          // Offset: start
        partition_size,             // Size: limited
        EXT_FLASH_PARTITION_LABEL,
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        &partition
    );
    
    if (err != ESP_OK || partition == NULL) {
        ESP_LOGE(TAG, "Failed to register partition: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Partition registered: %s, size: %lu bytes", 
             partition->label, partition->size);
    
    // SPIFFS configuration
    esp_vfs_spiffs_conf_t conf = {
        .base_path = FILE_MGR_BASE_PATH,
        .partition_label = EXT_FLASH_PARTITION_LABEL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount/format SPIFFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Partition not found: %s", EXT_FLASH_PARTITION_LABEL);
        } else {
            ESP_LOGE(TAG, "SPIFFS init failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }
    
    s_mounted = true;
    
    ESP_LOGI(TAG, "SPIFFS initialized successfully");
    file_manager_print_info();
    
    return ESP_OK;
}

esp_err_t file_manager_deinit(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    
    esp_err_t ret = esp_vfs_spiffs_unregister(EXT_FLASH_PARTITION_LABEL);
    if (ret == ESP_OK) {
        s_mounted = false;
    }
    return ret;
}

esp_err_t file_manager_write(const char *path, const void *data, size_t size)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!path || !data) return ESP_ERR_INVALID_ARG;
    
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open for write: %s (errno: %d)", path, errno);
        return ESP_FAIL;
    }
    
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    
    if (written != size) {
        ESP_LOGE(TAG, "Write error: %s (wrote %u, expected %u)", path, written, size);
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Written: %s (%u bytes)", path, written);
    return ESP_OK;
}

esp_err_t file_manager_read(const char *path, void *buffer, size_t size, size_t *read_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!path || !buffer) return ESP_ERR_INVALID_ARG;
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open for read: %s (errno: %d)", path, errno);
        return ESP_FAIL;
    }
    
    size_t rd = fread(buffer, 1, size, f);
    fclose(f);
    
    if (read_bytes) {
        *read_bytes = rd;
    }
    
    ESP_LOGD(TAG, "Read: %s (%u bytes)", path, rd);
    return ESP_OK;
}

bool file_manager_exists(const char *path)
{
    if (!s_mounted || !path) return false;
    
    struct stat st;
    return (stat(path, &st) == 0);
}

esp_err_t file_manager_delete(const char *path)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!path) return ESP_ERR_INVALID_ARG;
    
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "Failed to delete: %s (errno: %d)", path, errno);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Deleted: %s", path);
    return ESP_OK;
}

esp_err_t file_manager_mkdir(const char *path)
{
    // SPIFFS is flat - virtual directories via path separators
    (void)path;
    return ESP_OK;
}

esp_err_t file_manager_list_dir(const char *path, file_info_t *files, size_t max_files, size_t *count)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!path || !files) return ESP_ERR_INVALID_ARG;
    
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open dir: %s (errno: %d)", path, errno);
        return ESP_FAIL;
    }
    
    size_t idx = 0;
    struct dirent *entry;
    struct stat st;
    char full_path[FILE_MGR_MAX_PATH_LEN];
    
    while ((entry = readdir(dir)) != NULL && idx < max_files) {
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        
        strncpy(files[idx].name, entry->d_name, sizeof(files[idx].name) - 1);
        files[idx].name[sizeof(files[idx].name) - 1] = '\0';
        
        if (stat(full_path, &st) == 0) {
            files[idx].size = st.st_size;
            files[idx].is_dir = S_ISDIR(st.st_mode);
        } else {
            files[idx].size = 0;
            files[idx].is_dir = false;
        }
        
        idx++;
    }
    
    closedir(dir);
    
    if (count) {
        *count = idx;
    }
    
    return ESP_OK;
}

int32_t file_manager_get_size(const char *path)
{
    if (!s_mounted || !path) return -1;
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return st.st_size;
}

esp_err_t file_manager_get_info(uint32_t *total_bytes, uint32_t *used_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    
    size_t total = 0, used = 0;
    esp_err_t ret = esp_spiffs_info(EXT_FLASH_PARTITION_LABEL, &total, &used);
    
    if (ret == ESP_OK) {
        if (total_bytes) *total_bytes = total;
        if (used_bytes) *used_bytes = used;
    }
    
    return ret;
}

void file_manager_print_info(void)
{
    ESP_LOGI(TAG, "========== FILESYSTEM INFO ==========");
    
    if (!s_mounted) {
        ESP_LOGW(TAG, "Filesystem not mounted!");
        return;
    }
    
    uint32_t total = 0, used = 0;
    if (file_manager_get_info(&total, &used) == ESP_OK) {
        uint32_t free_bytes = total - used;
        float used_pct = (total > 0) ? (used * 100.0f / total) : 0;
        
        ESP_LOGI(TAG, "Total:    %lu bytes (%lu MB)", total, total / (1024 * 1024));
        ESP_LOGI(TAG, "Used:     %lu bytes (%lu KB)", used, used / 1024);
        ESP_LOGI(TAG, "Free:     %lu bytes (%lu MB)", free_bytes, free_bytes / (1024 * 1024));
        ESP_LOGI(TAG, "Usage:    %.1f%%", used_pct);
    }
    
    ESP_LOGI(TAG, "Mount:    %s", FILE_MGR_BASE_PATH);
    ESP_LOGI(TAG, "======================================");
}

esp_err_t file_manager_format(void)
{
    ESP_LOGW(TAG, "Formatting filesystem...");
    return esp_spiffs_format(EXT_FLASH_PARTITION_LABEL);
}

FILE* file_manager_fopen(const char *path, const char *mode)
{
    if (!s_mounted || !path || !mode) return NULL;
    return fopen(path, mode);
}

esp_err_t file_manager_write_string(const char *path, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    return file_manager_write(path, str, strlen(str));
}

esp_err_t file_manager_read_string(const char *path, char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) return ESP_ERR_INVALID_ARG;
    
    size_t read = 0;
    esp_err_t ret = file_manager_read(path, buffer, max_len - 1, &read);
    if (ret == ESP_OK) {
        buffer[read] = '\0';
    }
    return ret;
}

esp_err_t file_manager_append(const char *path, const void *data, size_t size)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!path || !data) return ESP_ERR_INVALID_ARG;
    
    FILE *f = fopen(path, "ab");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open for append: %s (errno: %d)", path, errno);
        return ESP_FAIL;
    }
    
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    
    if (written != size) {
        ESP_LOGE(TAG, "Append error: %s", path);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t file_manager_append_string(const char *path, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    return file_manager_append(path, str, strlen(str));
}

bool file_manager_is_mounted(void)
{
    return s_mounted;
}

esp_err_t file_manager_rename(const char *old_path, const char *new_path)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!old_path || !new_path) return ESP_ERR_INVALID_ARG;
    
    if (rename(old_path, new_path) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s to %s (errno: %d)", old_path, new_path, errno);
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Renamed: %s -> %s", old_path, new_path);
    return ESP_OK;
}
