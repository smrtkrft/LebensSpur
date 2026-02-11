/**
 * @file time_manager.h
 * @brief Time Manager - NTP Sync and Time Functions
 * 
 * Provides:
 * - NTP time synchronization
 * - Timezone configuration
 * - Local time functions
 * - Uptime tracking
 */

#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * NTP SERVERS
 * ============================================ */
#define NTP_SERVER_PRIMARY      "pool.ntp.org"
#define NTP_SERVER_SECONDARY    "time.google.com"
#define NTP_SERVER_TERTIARY     "time.cloudflare.com"

// Sync interval (seconds)
#define NTP_SYNC_INTERVAL_S     (3600)  // 1 hour

// Max wait for first sync (ms)
#define NTP_SYNC_TIMEOUT_MS     (30000)

/* ============================================
 * TIMEZONE DEFINITIONS
 * ============================================ */
// Common timezone strings (POSIX format)
#define TZ_UTC          "UTC0"
#define TZ_EUROPE_IST   "CET-1CEST,M3.5.0,M10.5.0/3"      // Istanbul
#define TZ_EUROPE_LON   "GMT0BST,M3.5.0/1,M10.5.0"        // London
#define TZ_EUROPE_PAR   "CET-1CEST,M3.5.0,M10.5.0/3"      // Paris/Berlin
#define TZ_US_PACIFIC   "PST8PDT,M3.2.0,M11.1.0"          // Los Angeles
#define TZ_US_EASTERN   "EST5EDT,M3.2.0,M11.1.0"          // New York
#define TZ_ASIA_TOKYO   "JST-9"                            // Tokyo

#define DEFAULT_TIMEZONE TZ_EUROPE_IST

/* ============================================
 * TIME STRUCTURES
 * ============================================ */

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int weekday;    // 0=Sunday
} time_info_t;

typedef struct {
    uint32_t days;
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;
} uptime_t;

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize time manager
 * @note Call after WiFi is connected for NTP sync
 */
esp_err_t time_manager_init(void);

/**
 * @brief Deinitialize time manager
 */
void time_manager_deinit(void);

/* ============================================
 * NTP SYNCHRONIZATION
 * ============================================ */

/**
 * @brief Start NTP synchronization
 * @note Non-blocking, runs in background
 */
esp_err_t time_manager_start_ntp(void);

/**
 * @brief Stop NTP synchronization
 */
void time_manager_stop_ntp(void);

/**
 * @brief Force NTP resync
 */
esp_err_t time_manager_sync_now(void);

/**
 * @brief Check if time is synchronized
 * @return true if synced with NTP
 */
bool time_manager_is_synced(void);

/**
 * @brief Wait for NTP sync (blocking)
 * @param timeout_ms Maximum wait time
 * @return true if synced before timeout
 */
bool time_manager_wait_sync(uint32_t timeout_ms);

/* ============================================
 * TIMEZONE
 * ============================================ */

/**
 * @brief Set timezone
 * @param tz POSIX timezone string
 */
void time_manager_set_timezone(const char *tz);

/**
 * @brief Get current timezone
 * @return Timezone string
 */
const char* time_manager_get_timezone(void);

/* ============================================
 * TIME FUNCTIONS
 * ============================================ */

/**
 * @brief Get current Unix timestamp (seconds since 1970)
 */
time_t time_manager_get_timestamp(void);

/**
 * @brief Get current Unix timestamp in milliseconds
 */
int64_t time_manager_get_timestamp_ms(void);

/**
 * @brief Get local time info
 * @param info Output time structure
 */
void time_manager_get_local(time_info_t *info);

/**
 * @brief Get UTC time info
 * @param info Output time structure
 */
void time_manager_get_utc(time_info_t *info);

/**
 * @brief Get formatted time string
 * @param buffer Output buffer
 * @param size Buffer size
 * @param format strftime format (NULL for ISO8601)
 */
void time_manager_get_string(char *buffer, size_t size, const char *format);

/**
 * @brief Get ISO8601 timestamp
 * @param buffer Output buffer (min 30 chars)
 * @param size Buffer size
 */
void time_manager_get_iso8601(char *buffer, size_t size);

/* ============================================
 * UPTIME
 * ============================================ */

/**
 * @brief Get system uptime
 * @param uptime Output structure
 */
void time_manager_get_uptime(uptime_t *uptime);

/**
 * @brief Get uptime in seconds
 */
uint32_t time_manager_get_uptime_seconds(void);

/**
 * @brief Get formatted uptime string (e.g., "2d 5h 30m 15s")
 * @param buffer Output buffer
 * @param size Buffer size
 */
void time_manager_get_uptime_string(char *buffer, size_t size);

/* ============================================
 * UTILITIES
 * ============================================ */

/**
 * @brief Check if time is within a range (daily window)
 * @param start_hhmm Start time "HH:MM"
 * @param end_hhmm End time "HH:MM"
 * @return true if current time is within range
 */
bool time_manager_is_in_window(const char *start_hhmm, const char *end_hhmm);

/**
 * @brief Parse HH:MM format
 * @param hhmm Time string
 * @param hours Output hours (0-23)
 * @param minutes Output minutes (0-59)
 * @return ESP_OK on success
 */
esp_err_t time_manager_parse_hhmm(const char *hhmm, int *hours, int *minutes);

/**
 * @brief Get day of week name
 * @param weekday 0=Sunday, 6=Saturday
 * @return Day name string
 */
const char* time_manager_get_day_name(int weekday);

#ifdef __cplusplus
}
#endif

#endif // TIME_MANAGER_H
