/**
 * External Flash Manager - W25Q256 (32MB)
 *
 * SPI2 üzerinden harici W25Q256 flash kontrolü.
 * Pin bağlantısı: CS=GPIO21, MISO=GPIO0, MOSI=GPIO22, SCLK=GPIO16
 *
 * Bağımlılık: Yok (sadece ESP-IDF SPI driver)
 * Katman: 0 (Donanım)
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

// Pin tanımları (PCB bağlantısı - Seeed XIAO ESP32-C6)
#define EXT_FLASH_CS_PIN    21  // D3
#define EXT_FLASH_MISO_PIN  0   // D0
#define EXT_FLASH_MOSI_PIN  22  // D4
#define EXT_FLASH_SCLK_PIN  16  // D6

// Flash özellikleri
#define EXT_FLASH_SIZE_MB       32
#define EXT_FLASH_SIZE_BYTES    (EXT_FLASH_SIZE_MB * 1024 * 1024)
#define EXT_FLASH_SECTOR_SIZE   4096

/**
 * Harici flash'ı başlat (SPI bus + flash device init)
 */
esp_err_t ext_flash_init(void);

/**
 * Harici flash'ı kapat ve SPI bus'ı serbest bırak
 */
esp_err_t ext_flash_deinit(void);

/**
 * esp_flash handle'ı al (filesystem mount için gerekli)
 */
esp_flash_t *ext_flash_get_handle(void);

/**
 * Algılanan flash boyutu (bytes)
 */
uint32_t ext_flash_get_size(void);

/**
 * Flash JEDEC ID'si (üretici + tip + kapasite)
 */
uint32_t ext_flash_get_id(void);

/**
 * Flash hazır mı?
 */
bool ext_flash_is_ready(void);

/**
 * Tüm flash'ı sil (dikkat: uzun sürer!)
 */
esp_err_t ext_flash_erase_chip(void);

/**
 * Belirli bir bölgeyi sil (4KB sektör hizalı)
 */
esp_err_t ext_flash_erase_region(uint32_t address, uint32_t size);

/**
 * Flash'tan oku
 */
esp_err_t ext_flash_read(uint32_t address, void *buffer, uint32_t size);

/**
 * Flash'a yaz (önceden silinmiş olmalı)
 */
esp_err_t ext_flash_write(uint32_t address, const void *buffer, uint32_t size);

/**
 * Debug bilgilerini yazdır
 */
void ext_flash_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // EXT_FLASH_H
