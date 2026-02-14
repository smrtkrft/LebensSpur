/**
 * File Manager - Harici Flash Dosya Sistemi (LittleFS)
 *
 * Harici W25Q256 (32MB) flash üzerinde LittleFS dosya sistemi.
 * LittleFS özellikleri:
 * - Gerçek dizin desteği
 * - Power-loss resilient (güç kesintisine dayanıklı)
 * - Wear leveling (flash aşınma dengeleme)
 * - Büyük partition desteği
 *
 * Mount noktası: /ext
 * Partition boyutu: 32MB (tam flash)
 *
 * Bağımlılık: ext_flash (Katman 0), joltwallet/littlefs component
 * Katman: 1 (Altyapı)
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

// Mount noktasi ve alt dizinler
#define FILE_MGR_BASE_PATH      "/ext"
#define FILE_MGR_LOG_PATH       "/ext/logs"
#define FILE_MGR_CONFIG_PATH    "/ext/config"
#define FILE_MGR_DATA_PATH      "/ext/data"

// Limitler
#define FILE_MGR_MAX_PATH_LEN   280
#define FILE_MGR_MAX_OPEN_FILES 10
#define FILE_MGR_PARTITION_SIZE (16 * 1024 * 1024)  // 16MB (SPI 3-byte adresleme limiti)

// Dosya bilgisi
typedef struct {
    char name[64];
    uint32_t size;
    bool is_dir;
} file_info_t;

/**
 * LittleFS dosya sistemini başlat (ext_flash_init sonrası çağrılmalı)
 * İlk açılışta otomatik format yapar
 */
esp_err_t file_manager_init(void);

/**
 * Dosya sistemini kapat
 */
esp_err_t file_manager_deinit(void);

/**
 * Dosya yaz (mevcut varsa üzerine yazar)
 */
esp_err_t file_manager_write(const char *path, const void *data, size_t size);

/**
 * String olarak yaz
 */
esp_err_t file_manager_write_string(const char *path, const char *str);

/**
 * Dosya oku
 * @param read_bytes Okunan byte sayısı (NULL olabilir)
 */
esp_err_t file_manager_read(const char *path, void *buffer, size_t size, size_t *read_bytes);

/**
 * String olarak oku (null-terminate eder)
 */
esp_err_t file_manager_read_string(const char *path, char *buffer, size_t max_len);

/**
 * Dosyanın sonuna ekle
 */
esp_err_t file_manager_append(const char *path, const void *data, size_t size);

/**
 * Dosya var mı
 */
bool file_manager_exists(const char *path);

/**
 * Dosya sil
 */
esp_err_t file_manager_delete(const char *path);

/**
 * Dosya boyutu (-1 = bulunamadı)
 */
int32_t file_manager_get_size(const char *path);

/**
 * Dizin oluştur (LittleFS gerçek dizin desteği sağlar)
 */
esp_err_t file_manager_mkdir(const char *path);

/**
 * Dizin içeriğini listele
 */
esp_err_t file_manager_list_dir(const char *path, file_info_t *files, size_t max_files, size_t *count);

/**
 * Dosya sistemi kullanım bilgisi
 */
esp_err_t file_manager_get_info(uint32_t *total_bytes, uint32_t *used_bytes);

/**
 * Dosya sistemi mount edilmiş mi
 */
bool file_manager_is_mounted(void);

/**
 * Dosya sistemini formatla (DİKKAT: tüm veriler silinir!)
 */
esp_err_t file_manager_format(void);

/**
 * stdio FILE* ile dosya aç
 */
FILE *file_manager_fopen(const char *path, const char *mode);

/**
 * Debug bilgilerini yazdır
 */
void file_manager_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // FILE_MANAGER_H
