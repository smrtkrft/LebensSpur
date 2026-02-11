/**
 * @file ota_manager.h
 * @brief OTA (Over-The-Air) Update Manager
 */

#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * OTA STATUS
 * ============================================ */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_WRITING,
    OTA_STATE_VERIFYING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
} ota_state_t;

typedef struct {
    ota_state_t state;
    int progress_percent;       // 0-100
    size_t bytes_received;
    size_t total_bytes;
    char error_message[64];
} ota_status_t;

/* ============================================
 * CALLBACKS
 * ============================================ */

/**
 * @brief Progress callback
 * @param progress Percent complete (0-100)
 */
typedef void (*ota_progress_cb_t)(int progress);

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize OTA manager
 */
esp_err_t ota_manager_init(void);

/**
 * @brief Set progress callback
 */
void ota_manager_set_progress_cb(ota_progress_cb_t cb);

/* ============================================
 * OTA OPERATIONS
 * ============================================ */

/**
 * @brief Start OTA from URL
 * @param url Firmware URL (.bin file)
 * @return ESP_OK if started
 */
esp_err_t ota_start_from_url(const char *url);

/**
 * @brief Start OTA from uploaded data
 * @param data Firmware data
 * @param size Data size
 * @return ESP_OK on success
 */
esp_err_t ota_start_from_data(const uint8_t *data, size_t size);

/**
 * @brief Abort ongoing OTA
 */
void ota_abort(void);

/* ============================================
 * STATUS
 * ============================================ */

/**
 * @brief Get current OTA status
 */
void ota_get_status(ota_status_t *status);

/**
 * @brief Check if OTA is in progress
 */
bool ota_is_in_progress(void);

/* ============================================
 * FIRMWARE INFO
 * ============================================ */

/**
 * @brief Get current firmware version
 * @param buffer Output buffer
 * @param size Buffer size
 */
void ota_get_version(char *buffer, size_t size);

/**
 * @brief Get current partition info
 * @param buffer Output buffer
 * @param size Buffer size
 */
void ota_get_partition_info(char *buffer, size_t size);

/**
 * @brief Restart device to apply update
 */
void ota_restart(void);

/**
 * @brief Rollback to previous firmware
 */
esp_err_t ota_rollback(void);

/**
 * @brief Mark current firmware as valid
 */
esp_err_t ota_mark_valid(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_MANAGER_H
