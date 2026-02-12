/**
 * @file file_manager.c
 * @brief File System Manager - LittleFS on External Flash (3 Partitions)
 *
 * Partition layout on W25Q256 (32MB):
 *   0x0000000 - 1MB  "settings"    → /cfg
 *   0x0100000 - 4MB  "gui_storage" → /gui
 *   0x0500000 - 27MB "user_data"   → /data
 */

#include "file_manager.h"
#include "ext_flash.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

static const char *TAG = "file_mgr";

/* ============================================
 * PARTITION DEFINITIONS
 * ============================================ */

typedef struct {
    const char *label;          // Partition label
    const char *mount_point;    // VFS mount point
    uint32_t offset;            // Offset on external flash
    uint32_t size;              // Partition size in bytes
    int max_files;              // Max simultaneously open files
    bool mounted;               // Mount status
} partition_def_t;

static partition_def_t s_partitions[] = {
    {
        .label = "settings",
        .mount_point = FILE_MGR_SETTINGS_MOUNT,
        .offset = 0x000000,
        .size = 1 * 1024 * 1024,        // 1MB
        .max_files = 5,
        .mounted = false,
    },
    {
        .label = "gui_storage",
        .mount_point = FILE_MGR_GUI_MOUNT,
        .offset = 0x100000,
        .size = 4 * 1024 * 1024,        // 4MB
        .max_files = 5,
        .mounted = false,
    },
    {
        .label = "user_data",
        .mount_point = FILE_MGR_DATA_MOUNT,
        .offset = 0x500000,
        .size = 27 * 1024 * 1024,       // 27MB
        .max_files = 5,
        .mounted = false,
    },
};

#define PARTITION_COUNT (sizeof(s_partitions) / sizeof(s_partitions[0]))

/* ============================================
 * PRIVATE HELPERS
 * ============================================ */

static const char* get_partition_label(const char *mount_point)
{
    for (int i = 0; i < PARTITION_COUNT; i++) {
        if (strcmp(s_partitions[i].mount_point, mount_point) == 0) {
            return s_partitions[i].label;
        }
    }
    return NULL;
}

static const char* get_mount_from_path(const char *path)
{
    if (!path) return NULL;

    for (int i = 0; i < PARTITION_COUNT; i++) {
        size_t len = strlen(s_partitions[i].mount_point);
        if (strncmp(path, s_partitions[i].mount_point, len) == 0 &&
            (path[len] == '/' || path[len] == '\0')) {
            return s_partitions[i].mount_point;
        }
    }
    return NULL;
}

/* ============================================
 * INITIALIZATION
 * ============================================ */

esp_err_t file_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing LittleFS on external flash (3 partitions)...");

    if (!ext_flash_is_ready()) {
        ESP_LOGE(TAG, "External flash not ready!");
        return ESP_ERR_INVALID_STATE;
    }

    esp_flash_t *flash = ext_flash_get_handle();
    if (!flash) {
        ESP_LOGE(TAG, "Could not get flash handle");
        return ESP_ERR_INVALID_STATE;
    }

    int mounted_count = 0;

    for (int i = 0; i < PARTITION_COUNT; i++) {
        partition_def_t *p = &s_partitions[i];

        if (p->mounted) {
            mounted_count++;
            continue;
        }

        ESP_LOGI(TAG, "Registering partition '%s': offset=0x%06lX, size=%luKB, mount=%s",
                 p->label, (unsigned long)p->offset,
                 (unsigned long)(p->size / 1024), p->mount_point);

        // Register external flash partition
        const esp_partition_t *partition = NULL;
        esp_err_t err = esp_partition_register_external(
            flash,
            p->offset,
            p->size,
            p->label,
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_ANY,
            &partition
        );

        if (err != ESP_OK || partition == NULL) {
            ESP_LOGE(TAG, "Failed to register partition '%s': %s", p->label, esp_err_to_name(err));
            continue;
        }

        // Mount LittleFS
        esp_vfs_littlefs_conf_t conf = {
            .base_path = p->mount_point,
            .partition_label = p->label,
            .format_if_mount_failed = true,
            .dont_mount = false,
        };

        err = esp_vfs_littlefs_register(&conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount LittleFS on '%s': %s", p->label, esp_err_to_name(err));
            continue;
        }

        p->mounted = true;
        mounted_count++;
        ESP_LOGI(TAG, "Partition '%s' mounted at %s", p->label, p->mount_point);
    }

    if (mounted_count == 0) {
        ESP_LOGE(TAG, "No partitions mounted!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LittleFS initialized: %d/%d partitions mounted", mounted_count, PARTITION_COUNT);
    file_manager_print_info();

    return ESP_OK;
}

esp_err_t file_manager_deinit(void)
{
    for (int i = 0; i < PARTITION_COUNT; i++) {
        if (s_partitions[i].mounted) {
            esp_vfs_littlefs_unregister(s_partitions[i].label);
            s_partitions[i].mounted = false;
        }
    }
    return ESP_OK;
}

/* ============================================
 * FILE OPERATIONS
 * ============================================ */

esp_err_t file_manager_write(const char *path, const void *data, size_t size)
{
    if (!path || !data) return ESP_ERR_INVALID_ARG;
    if (!get_mount_from_path(path)) return ESP_ERR_INVALID_ARG;

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
    if (!path) return false;

    struct stat st;
    return (stat(path, &st) == 0);
}

esp_err_t file_manager_delete(const char *path)
{
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
    if (!path) return ESP_ERR_INVALID_ARG;

    // LittleFS supports real directories
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to mkdir: %s (errno: %d)", path, errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t file_manager_list_dir(const char *path, file_info_t *files, size_t max_files, size_t *count)
{
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
            files[idx].is_dir = (entry->d_type == DT_DIR);
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
    if (!path) return -1;

    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return st.st_size;
}

/* ============================================
 * FILESYSTEM INFO
 * ============================================ */

esp_err_t file_manager_get_info(uint32_t *total_bytes, uint32_t *used_bytes)
{
    uint32_t total_sum = 0, used_sum = 0;

    for (int i = 0; i < PARTITION_COUNT; i++) {
        if (!s_partitions[i].mounted) continue;

        size_t total = 0, used = 0;
        if (esp_littlefs_info(s_partitions[i].label, &total, &used) == ESP_OK) {
            total_sum += total;
            used_sum += used;
        }
    }

    if (total_bytes) *total_bytes = total_sum;
    if (used_bytes) *used_bytes = used_sum;

    return ESP_OK;
}

esp_err_t file_manager_get_partition_info(const char *mount_point, uint32_t *total_bytes, uint32_t *used_bytes)
{
    if (!mount_point) return ESP_ERR_INVALID_ARG;

    const char *label = get_partition_label(mount_point);
    if (!label) return ESP_ERR_NOT_FOUND;

    size_t total = 0, used = 0;
    esp_err_t ret = esp_littlefs_info(label, &total, &used);

    if (ret == ESP_OK) {
        if (total_bytes) *total_bytes = total;
        if (used_bytes) *used_bytes = used;
    }

    return ret;
}

void file_manager_print_info(void)
{
    ESP_LOGI(TAG, "========== FILESYSTEM INFO (LittleFS) ==========");

    uint32_t grand_total = 0, grand_used = 0;

    for (int i = 0; i < PARTITION_COUNT; i++) {
        partition_def_t *p = &s_partitions[i];

        if (!p->mounted) {
            ESP_LOGW(TAG, "  [%s] %s - NOT MOUNTED", p->label, p->mount_point);
            continue;
        }

        size_t total = 0, used = 0;
        esp_littlefs_info(p->label, &total, &used);

        uint32_t free_bytes = total - used;
        float pct = (total > 0) ? (used * 100.0f / total) : 0;

        ESP_LOGI(TAG, "  [%s] %s: %luKB total, %luKB used, %luKB free (%.1f%%)",
                 p->label, p->mount_point,
                 (unsigned long)(total / 1024),
                 (unsigned long)(used / 1024),
                 (unsigned long)(free_bytes / 1024),
                 pct);

        grand_total += total;
        grand_used += used;
    }

    ESP_LOGI(TAG, "  ─────────────────────────────────────────");
    ESP_LOGI(TAG, "  Total: %luKB, Used: %luKB, Free: %luKB",
             (unsigned long)(grand_total / 1024),
             (unsigned long)(grand_used / 1024),
             (unsigned long)((grand_total - grand_used) / 1024));
    ESP_LOGI(TAG, "================================================");
}

/* ============================================
 * FORMAT
 * ============================================ */

esp_err_t file_manager_format(void)
{
    ESP_LOGW(TAG, "Formatting all partitions...");

    esp_err_t last_err = ESP_OK;
    for (int i = 0; i < PARTITION_COUNT; i++) {
        if (!s_partitions[i].mounted) continue;

        esp_err_t ret = esp_littlefs_format(s_partitions[i].label);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to format '%s': %s", s_partitions[i].label, esp_err_to_name(ret));
            last_err = ret;
        } else {
            ESP_LOGI(TAG, "Formatted: %s", s_partitions[i].label);
        }
    }

    return last_err;
}

/* ============================================
 * CONVENIENCE FUNCTIONS
 * ============================================ */

FILE* file_manager_fopen(const char *path, const char *mode)
{
    if (!path || !mode) return NULL;
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
    for (int i = 0; i < PARTITION_COUNT; i++) {
        if (!s_partitions[i].mounted) return false;
    }
    return true;
}

esp_err_t file_manager_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return ESP_ERR_INVALID_ARG;

    if (rename(old_path, new_path) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s to %s (errno: %d)", old_path, new_path, errno);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Renamed: %s -> %s", old_path, new_path);
    return ESP_OK;
}
