/**
 * Time Manager - NTP Zaman Senkronizasyonu
 *
 * Zürich timezone: CET (UTC+1) / CEST (UTC+2 yaz saati)
 * NTP sunucuları: pool.ntp.org, time.google.com, time.cloudflare.com
 *
 * Bağımlılık: Yok (sadece ESP-IDF SNTP)
 * Katman: 1 (Altyapı)
 */

#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Zaman formatları
#define TIME_FORMAT_FULL    "%Y-%m-%d %H:%M:%S"   // 2026-02-13 14:30:45
#define TIME_FORMAT_DATE    "%Y-%m-%d"             // 2026-02-13
#define TIME_FORMAT_TIME    "%H:%M:%S"             // 14:30:45
#define TIME_FORMAT_LOG     "%d.%m.%Y %H:%M:%S"   // 13.02.2026 14:30:45

// Buffer boyutları
#define TIME_STR_MAX_LEN    64
#define UPTIME_STR_MAX_LEN  48

/**
 * Timezone'u ayarla (Zürich CET/CEST)
 * WiFi'den önce çağrılabilir
 */
esp_err_t time_manager_init(void);

/**
 * NTP senkronizasyonunu başlat (internet bağlantısı gerekli)
 */
esp_err_t time_manager_sync(void);

/**
 * NTP'yi durdur
 */
void time_manager_stop(void);

/**
 * NTP senkronize oldu mu
 */
bool time_manager_is_synced(void);

/**
 * Güncel zamanı al (struct tm)
 * @return ESP_ERR_INVALID_STATE: henüz senkronize değil (2020 öncesi tarih)
 */
esp_err_t time_manager_get_time(struct tm *timeinfo);

/**
 * Zamanı string olarak al
 * Senkronize ise formatlanmış tarih, değilse "---"
 * @param format NULL ise TIME_FORMAT_LOG kullanılır
 */
void time_manager_get_time_str(char *buffer, size_t len, const char *format);

/**
 * Uptime (milisaniye)
 */
int64_t time_manager_get_uptime_ms(void);

/**
 * Uptime (saniye)
 */
uint32_t time_manager_get_uptime_sec(void);

/**
 * Geçen süreyi okunabilir string'e çevir
 * Örn: "3 saat 12 dakika önce"
 */
void time_manager_get_elapsed_str(int64_t timestamp_ms, char *buffer, size_t len);

/**
 * Log satırı için zaman string'i
 * NTP varsa: gerçek tarih/saat
 * NTP yoksa: uptime formatı
 */
void time_manager_get_log_time_str(char *buffer, size_t len);

/**
 * Debug bilgilerini yazdır
 */
void time_manager_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // TIME_MANAGER_H
