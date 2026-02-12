/**
 * @file ext_flash.h
 * @brief External Flash Manager - W25Q256 (32MB)
 * 
 * Manages external SPI flash for storing:
 * - Settings & backups (/cfg)     - 1MB
 * - Web GUI files & logs (/gui)   - 4MB
 * - User data & attachments (/data) - 27MB
 */

#ifndef EXT_FLASH_H
#define EXT_FLASH_H

#include "esp_err.h"
#include "esp_flash.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** External Flash SPI Pin Definitions (XIAO ESP32-C6 PCB) */
#define EXT_FLASH_CS_PIN    21   // D3 = GPIO21
#define EXT_FLASH_MISO_PIN  0    // D0 = GPIO0
#define EXT_FLASH_MOSI_PIN  22   // D4 = GPIO22
#define EXT_FLASH_SCLK_PIN  16   // D6 = GPIO16

/** Flash Properties */
#define EXT_FLASH_SIZE_MB   32
#define EXT_FLASH_SIZE_BYTES (EXT_FLASH_SIZE_MB * 1024 * 1024)

/**
 * @brief Initialize external flash
 * @return ESP_OK on success
 */
esp_err_t ext_flash_init(void);

/**
 * @brief Get flash handle for direct operations
 * @return Flash handle or NULL if not initialized
 */
esp_flash_t* ext_flash_get_handle(void);

/**
 * @brief Get flash size in bytes
 * @return Flash size or 0 if not initialized
 */
uint32_t ext_flash_get_size(void);

/**
 * @brief Get flash JEDEC ID
 * @return Flash ID or 0 if not initialized
 */
uint32_t ext_flash_get_id(void);

/**
 * @brief Erase entire flash chip
 * @warning This takes a long time (~1-2 minutes for 32MB)
 * @return ESP_OK on success
 */
esp_err_t ext_flash_erase_chip(void);

/**
 * @brief Erase a 4KB sector
 * @param address Sector-aligned address
 * @return ESP_OK on success
 */
esp_err_t ext_flash_erase_sector(uint32_t address);

/**
 * @brief Read data from flash
 * @param address Source address
 * @param buffer Destination buffer
 * @param size Number of bytes to read
 * @return ESP_OK on success
 */
esp_err_t ext_flash_read(uint32_t address, void *buffer, uint32_t size);

/**
 * @brief Write data to flash
 * @note Address must be erased before writing
 * @param address Destination address
 * @param buffer Source buffer
 * @param size Number of bytes to write
 * @return ESP_OK on success
 */
esp_err_t ext_flash_write(uint32_t address, const void *buffer, uint32_t size);

/**
 * @brief Check if flash is initialized and ready
 * @return true if ready
 */
bool ext_flash_is_ready(void);

/**
 * @brief Print flash information to log
 */
void ext_flash_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // EXT_FLASH_H
