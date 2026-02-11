/**
 * @file file_manager.h
 * @brief File System Manager - SPIFFS on External Flash
 * 
 * Manages filesystem on external W25Q256 flash.
 * 
 * Directory Structure:
 * /ext/web     - GUI files (HTML, CSS, JS)
 * /ext/config  - JSON configuration files
 * /ext/logs    - System and audit logs
 * /ext/data    - User data, attachments
 */

#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Mount points and paths */
#define FILE_MGR_BASE_PATH      "/ext"
#define FILE_MGR_WEB_PATH       "/ext/web"
#define FILE_MGR_CONFIG_PATH    "/ext/config"
#define FILE_MGR_LOG_PATH       "/ext/logs"
#define FILE_MGR_DATA_PATH      "/ext/data"

/** Filesystem limits */
#define FILE_MGR_MAX_PATH_LEN   280
#define FILE_MGR_MAX_FILES      256

/** File info structure */
typedef struct {
    char name[64];
    uint32_t size;
    bool is_dir;
} file_info_t;

/**
 * @brief Initialize filesystem
 * Must be called after ext_flash_init()
 * @return ESP_OK on success
 */
esp_err_t file_manager_init(void);

/**
 * @brief Deinitialize filesystem
 * @return ESP_OK on success
 */
esp_err_t file_manager_deinit(void);

/**
 * @brief Write data to file
 * @param path Full file path (e.g., /ext/config/timer.json)
 * @param data Data to write
 * @param size Data size in bytes
 * @return ESP_OK on success
 */
esp_err_t file_manager_write(const char *path, const void *data, size_t size);

/**
 * @brief Read data from file
 * @param path Full file path
 * @param buffer Destination buffer
 * @param size Buffer size
 * @param read_bytes Actual bytes read (output)
 * @return ESP_OK on success
 */
esp_err_t file_manager_read(const char *path, void *buffer, size_t size, size_t *read_bytes);

/**
 * @brief Check if file exists
 * @param path File path
 * @return true if exists
 */
bool file_manager_exists(const char *path);

/**
 * @brief Delete a file
 * @param path File path
 * @return ESP_OK on success
 */
esp_err_t file_manager_delete(const char *path);

/**
 * @brief Create directory (virtual in SPIFFS)
 * @param path Directory path
 * @return ESP_OK on success
 */
esp_err_t file_manager_mkdir(const char *path);

/**
 * @brief List directory contents
 * @param path Directory path
 * @param files Array to store file info
 * @param max_files Maximum number of files
 * @param count Number of files found (output)
 * @return ESP_OK on success
 */
esp_err_t file_manager_list_dir(const char *path, file_info_t *files, size_t max_files, size_t *count);

/**
 * @brief Get file size
 * @param path File path
 * @return File size or -1 on error
 */
int32_t file_manager_get_size(const char *path);

/**
 * @brief Get filesystem info
 * @param total_bytes Total space (output)
 * @param used_bytes Used space (output)
 * @return ESP_OK on success
 */
esp_err_t file_manager_get_info(uint32_t *total_bytes, uint32_t *used_bytes);

/**
 * @brief Print filesystem info to log
 */
void file_manager_print_info(void);

/**
 * @brief Format filesystem
 * @warning Erases all data!
 * @return ESP_OK on success
 */
esp_err_t file_manager_format(void);

/**
 * @brief Open file with standard FILE handle
 * @param path File path
 * @param mode File mode ("r", "w", "a", etc.)
 * @return FILE handle or NULL on error
 */
FILE* file_manager_fopen(const char *path, const char *mode);

/**
 * @brief Write string to file
 * @param path File path
 * @param str Null-terminated string
 * @return ESP_OK on success
 */
esp_err_t file_manager_write_string(const char *path, const char *str);

/**
 * @brief Read file as string
 * @param path File path
 * @param buffer Destination buffer
 * @param max_len Maximum buffer length
 * @return ESP_OK on success
 */
esp_err_t file_manager_read_string(const char *path, char *buffer, size_t max_len);

/**
 * @brief Append data to file
 * @param path File path
 * @param data Data to append
 * @param size Data size
 * @return ESP_OK on success
 */
esp_err_t file_manager_append(const char *path, const void *data, size_t size);

/**
 * @brief Append string to file
 * @param path File path
 * @param str String to append
 * @return ESP_OK on success
 */
esp_err_t file_manager_append_string(const char *path, const char *str);

/**
 * @brief Check if filesystem is mounted
 * @return true if mounted
 */
bool file_manager_is_mounted(void);

/**
 * @brief Rename a file
 * @param old_path Current file path
 * @param new_path New file path
 * @return ESP_OK on success
 */
esp_err_t file_manager_rename(const char *old_path, const char *new_path);

#ifdef __cplusplus
}
#endif

#endif // FILE_MANAGER_H
