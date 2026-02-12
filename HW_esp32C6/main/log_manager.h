/**
 * @file log_manager.h
 * @brief Log Manager - Event Logging System
 * 
 * Provides:
 * - Event logging to external flash
 * - Log rotation (max file size)
 * - Log categories (system, timer, security, mail)
 * - JSON/text export
 */

#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * CONFIGURATION
 * ============================================ */
#define LOG_BASE_PATH           "/gui/logs"
#define LOG_EVENT_FILE          "/gui/logs/events.log"
#define LOG_ARCHIVE_FILE        "/gui/logs/events.old"

#define LOG_MAX_FILE_SIZE       (64 * 1024)     // 64KB per file
#define LOG_MAX_ENTRIES         500             // Max entries before rotation
#define LOG_ENTRY_MAX_LEN       256             // Max message length

/* ============================================
 * LOG LEVELS
 * ============================================ */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL
} log_level_t;

/* ============================================
 * LOG CATEGORIES
 * ============================================ */
typedef enum {
    LOG_CAT_SYSTEM = 0,     // General system events
    LOG_CAT_TIMER,          // Dead man's switch events
    LOG_CAT_SECURITY,       // Login, auth events
    LOG_CAT_MAIL,           // Email events
    LOG_CAT_NETWORK,        // WiFi, connectivity
    LOG_CAT_CONFIG,         // Configuration changes
    LOG_CAT_RELAY,          // Relay actions
    LOG_CAT_USER,           // User actions
    LOG_CAT_MAX
} log_category_t;

/* ============================================
 * LOG ENTRY
 * ============================================ */
typedef struct {
    int64_t timestamp;          // Unix timestamp (ms)
    log_level_t level;          // Log level
    log_category_t category;    // Category
    char message[LOG_ENTRY_MAX_LEN];   // Message
    char source[32];            // Source (IP, device, etc)
} log_entry_t;

/* ============================================
 * LOG FILTER
 * ============================================ */
typedef struct {
    log_level_t min_level;      // Minimum level to include
    log_category_t category;    // Category filter (-1 for all)
    int64_t from_timestamp;     // From time (0 for all)
    int64_t to_timestamp;       // To time (0 for now)
    uint32_t max_entries;       // Max entries to return
    uint32_t offset;            // Skip first N entries
} log_filter_t;

#define LOG_FILTER_DEFAULT() { \
    .min_level = LOG_LEVEL_INFO, \
    .category = -1, \
    .from_timestamp = 0, \
    .to_timestamp = 0, \
    .max_entries = 100, \
    .offset = 0 \
}

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize log manager
 */
esp_err_t log_manager_init(void);

/**
 * @brief Deinitialize log manager
 */
void log_manager_deinit(void);

/* ============================================
 * LOGGING FUNCTIONS
 * ============================================ */

/**
 * @brief Log an event
 * @param level Log level
 * @param category Category
 * @param source Source identifier (can be NULL)
 * @param format Printf-style format string
 */
void log_event(log_level_t level, log_category_t category, 
               const char *source, const char *format, ...) __attribute__((format(printf, 4, 5)));

/**
 * @brief Log a system event
 */
#define LOG_SYSTEM(level, msg, ...) log_event(level, LOG_CAT_SYSTEM, NULL, msg, ##__VA_ARGS__)

/**
 * @brief Log a timer event
 */
#define LOG_TIMER(level, msg, ...) log_event(level, LOG_CAT_TIMER, NULL, msg, ##__VA_ARGS__)

/**
 * @brief Log a security event
 */
#define LOG_SECURITY(level, src, msg, ...) log_event(level, LOG_CAT_SECURITY, src, msg, ##__VA_ARGS__)

/**
 * @brief Log a mail event
 */
#define LOG_MAIL(level, msg, ...) log_event(level, LOG_CAT_MAIL, NULL, msg, ##__VA_ARGS__)

/**
 * @brief Log a network event
 */
#define LOG_NETWORK(level, msg, ...) log_event(level, LOG_CAT_NETWORK, NULL, msg, ##__VA_ARGS__)

/**
 * @brief Log a config event
 */
#define LOG_CONFIG(level, msg, ...) log_event(level, LOG_CAT_CONFIG, NULL, msg, ##__VA_ARGS__)

/**
 * @brief Log a relay event
 */
#define LOG_RELAY(level, msg, ...) log_event(level, LOG_CAT_RELAY, NULL, msg, ##__VA_ARGS__)

/**
 * @brief Log a user action
 */
#define LOG_USER(level, src, msg, ...) log_event(level, LOG_CAT_USER, src, msg, ##__VA_ARGS__)

/* ============================================
 * READING LOGS
 * ============================================ */

/**
 * @brief Get log entries as JSON string
 * @param filter Filter criteria
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of bytes written
 */
size_t log_get_entries_json(const log_filter_t *filter, char *buffer, size_t buffer_size);

/**
 * @brief Get log count
 * @return Total number of log entries
 */
uint32_t log_get_count(void);

/**
 * @brief Get log file size in bytes
 */
int32_t log_get_file_size(void);

/* ============================================
 * MAINTENANCE
 * ============================================ */

/**
 * @brief Force log rotation
 */
esp_err_t log_rotate(void);

/**
 * @brief Clear all logs
 */
esp_err_t log_clear(void);

/**
 * @brief Export logs to buffer (text format)
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Bytes written
 */
size_t log_export_text(char *buffer, size_t buffer_size);

/* ============================================
 * UTILITIES
 * ============================================ */

/**
 * @brief Get level name string
 */
const char* log_level_name(log_level_t level);

/**
 * @brief Get category name string
 */
const char* log_category_name(log_category_t category);

#ifdef __cplusplus
}
#endif

#endif // LOG_MANAGER_H
