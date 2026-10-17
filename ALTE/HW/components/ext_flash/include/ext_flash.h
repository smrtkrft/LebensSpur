#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * Harici SPI flash baslatma (16 MB veya 32 MB cip; boyut calisma aninda okunur)
 * 3 Partition Yapisi:
 *   Slot A    : 0x000000 (2 MB)      - GUI Web Arayuzu (LittleFS, /slot_a)
 *   Slot B    : 0x200000 (2 MB)      - GUI OTA slot (LittleFS, /slot_b)
 *   User Data : 0x400000 (cip sonu)  - Kullanici verileri (LittleFS, /ext)
 */
esp_err_t ext_flash_init(void);

// Slot A dosya islemleri
long ext_flash_slot_a_get_file_size(const char *path);

typedef struct ext_flash_file *ext_flash_file_t;
ext_flash_file_t ext_flash_slot_a_fopen(const char *path, long *out_size);
int ext_flash_slot_a_fread(ext_flash_file_t f, char *buf, size_t buf_size);
void ext_flash_slot_a_fclose(ext_flash_file_t f);

// Aktif slot dosya islemleri (hangi slot aktifse oradan okur)
ext_flash_file_t ext_flash_active_fopen(const char *path, long *out_size);
int ext_flash_active_fread(ext_flash_file_t f, char *buf, size_t buf_size);
void ext_flash_active_fclose(ext_flash_file_t f);
long ext_flash_active_get_file_size(const char *path);

// Aktif slot yonetimi
char ext_flash_get_active_slot(void);
esp_err_t ext_flash_set_active_slot(char slot);
const char *ext_flash_get_active_mount(void);
const char *ext_flash_get_inactive_mount(void);

// Durum sorgulama
bool ext_flash_is_mounted(void);
uint32_t ext_flash_get_chip_size(void);   // byte, 0 = cip yanit vermedi
bool ext_flash_is_slot_a_mounted(void);
bool ext_flash_is_slot_b_mounted(void);
bool ext_flash_is_user_data_mounted(void);

// Alan bilgisi
esp_err_t ext_flash_get_info(size_t *total, size_t *free_space);
esp_err_t ext_flash_get_slot_a_info(size_t *total, size_t *free_space);
esp_err_t ext_flash_get_slot_b_info(size_t *total, size_t *free_space);

// Bakim
esp_err_t ext_flash_format_user_data(void);

// GUI dosyalarini Slot A'ya yaz (firmware guncelleme sonrasi)
esp_err_t ext_flash_create_default_files(void);
