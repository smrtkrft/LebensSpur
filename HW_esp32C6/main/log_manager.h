/**
 * Log Manager - Harici Flash'a Log Yazma
 *
 * LittleFS üzerinde /ext/logs/ dizinine log dosyaları yazar.
 * Timestamp tabanlı dosya adları, otomatik rotasyon.
 *
 * Bağımlılık: file_manager (Katman 1)
 * Katman: 1 (Altyapı)
 */

#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include "esp_err.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Log ayarları
#define LOG_MGR_MAX_FILE_SIZE   (512 * 1024)    // 512KB per file
#define LOG_MGR_MAX_FILES       8               // Max rotasyon dosyası
#define LOG_MGR_BUFFER_SIZE     256             // Satır buffer

// Log seviyeleri (ESP_LOG ile uyumlu)
typedef enum {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_INFO = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_VERBOSE = 5
} log_level_t;

/**
 * Log sistemini başlat (file_manager_init sonrası)
 */
esp_err_t log_manager_init(void);

/**
 * Log sistemini kapat
 */
esp_err_t log_manager_deinit(void);

/**
 * Log seviyesini ayarla
 */
void log_manager_set_level(log_level_t level);

/**
 * Log yaz (printf format)
 */
void log_manager_write(log_level_t level, const char *tag, const char *format, ...);

// Kısayol makrolar
#define LOG_E(tag, fmt, ...) log_manager_write(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) log_manager_write(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) log_manager_write(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...) log_manager_write(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)

/**
 * Mevcut log dosyasının yolu
 */
const char *log_manager_get_current_file(void);

/**
 * Log dosyalarını listele
 */
esp_err_t log_manager_list_files(char files[][64], size_t max_files, size_t *count);

/**
 * Belirli bir log dosyasını oku
 */
esp_err_t log_manager_read_file(const char *filename, char *buffer, size_t max_size, size_t *read_size);

/**
 * Tüm log dosyalarını sil
 */
esp_err_t log_manager_clear_all(void);

/**
 * Log rotasyonu (eski dosyaları temizle, yeni dosya oluştur)
 */
esp_err_t log_manager_rotate(void);

/**
 * İstatistikleri al
 */
esp_err_t log_manager_get_stats(uint32_t *total_size, uint32_t *file_count);

/**
 * Buffer'ı diske yaz
 */
esp_err_t log_manager_flush(void);

/**
 * Debug bilgilerini yazdır
 */
void log_manager_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // LOG_MANAGER_H
