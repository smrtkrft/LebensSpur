/**
 * @file device_id.h
 * @brief Device ID management for LebensSpur ESP32-C6
 * 
 * Generates unique device ID from ESP32-C6 chip ID in format: LS-XXXXXXXXXX
 */

#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Device ID prefix */
#define DEVICE_ID_PREFIX "LS-"

/** Maximum device ID length (LS- + 12 chars + null) */
#define DEVICE_ID_MAX_LEN 16

/**
 * @brief Initialize device ID module
 * 
 * Reads ESP32-C6 chip ID and generates device ID.
 * Must be called once at startup.
 * 
 * @return ESP_OK on success, error code otherwise
 */
int device_id_init(void);

/**
 * @brief Get the device ID string
 * 
 * Returns pointer to device ID in format "LS-XXXXXXXXXX"
 * where XXXXXXXXXX is derived from chip's unique ID.
 * 
 * @return Pointer to null-terminated device ID string
 */
const char* device_id_get(void);

/**
 * @brief Get raw chip ID (MAC address based)
 * 
 * @param[out] chip_id Buffer to store 6-byte chip ID
 * @return ESP_OK on success
 */
int device_id_get_chip_id(uint8_t chip_id[6]);

/**
 * @brief Get device ID as uppercase hex string (without prefix)
 * 
 * @return Pointer to hex string (12 characters)
 */
const char* device_id_get_hex(void);

/**
 * @brief Check if device ID is valid/initialized
 * 
 * @return true if initialized, false otherwise
 */
bool device_id_is_valid(void);

#ifdef __cplusplus
}
#endif

#endif // DEVICE_ID_H
