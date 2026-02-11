/**
 * @file log_manager.c
 * @brief Log Manager Implementation
 */

#include "log_manager.h"
#include "file_manager.h"
#include "time_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "log_mgr";

/* ============================================
 * LEVEL & CATEGORY NAMES
 * ============================================ */
static const char *s_level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"
};

static const char *s_category_names[] = {
    "SYSTEM", "TIMER", "SECURITY", "MAIL", 
    "NETWORK", "CONFIG", "RELAY", "USER"
};

/* ============================================
 * INITIALIZATION
 * ============================================ */
esp_err_t log_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing log manager...");
    
    // Create logs directory
    file_manager_mkdir(LOG_BASE_PATH);
    
    // Check if log file exists
    if (!file_manager_exists(LOG_EVENT_FILE)) {
        // Create empty log file
        file_manager_write(LOG_EVENT_FILE, "", 0);
        ESP_LOGI(TAG, "Created new log file");
    }
    
    ESP_LOGI(TAG, "Log manager initialized");
    return ESP_OK;
}

void log_manager_deinit(void)
{
    ESP_LOGI(TAG, "Log manager deinit");
}

/* ============================================
 * LOGGING
 * ============================================ */
void log_event(log_level_t level, log_category_t category, 
               const char *source, const char *format, ...)
{
    // Validate inputs
    if (level >= sizeof(s_level_names)/sizeof(s_level_names[0])) {
        level = LOG_LEVEL_INFO;
    }
    if (category >= LOG_CAT_MAX) {
        category = LOG_CAT_SYSTEM;
    }
    
    // Format message
    char message[LOG_ENTRY_MAX_LEN];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Get timestamp
    char timestamp[30];
    time_manager_get_iso8601(timestamp, sizeof(timestamp));
    
    // Format log line
    char log_line[512];
    int len;
    if (source && source[0]) {
        len = snprintf(log_line, sizeof(log_line), 
                       "%s|%s|%s|%s|%s\n",
                       timestamp, 
                       s_level_names[level], 
                       s_category_names[category],
                       source,
                       message);
    } else {
        len = snprintf(log_line, sizeof(log_line), 
                       "%s|%s|%s||%s\n",
                       timestamp, 
                       s_level_names[level], 
                       s_category_names[category],
                       message);
    }
    
    // Also output to ESP-IDF log
    switch (level) {
        case LOG_LEVEL_DEBUG:
            ESP_LOGD(TAG, "[%s] %s", s_category_names[category], message);
            break;
        case LOG_LEVEL_INFO:
            ESP_LOGI(TAG, "[%s] %s", s_category_names[category], message);
            break;
        case LOG_LEVEL_WARN:
            ESP_LOGW(TAG, "[%s] %s", s_category_names[category], message);
            break;
        case LOG_LEVEL_ERROR:
        case LOG_LEVEL_CRITICAL:
            ESP_LOGE(TAG, "[%s] %s", s_category_names[category], message);
            break;
        default:
            break;
    }
    
    // Check file size before writing
    int32_t file_size = log_get_file_size();
    if (file_size > LOG_MAX_FILE_SIZE) {
        log_rotate();
    }
    
    // Append to log file
    file_manager_append(LOG_EVENT_FILE, log_line, len);
}

/* ============================================
 * READING LOGS
 * ============================================ */
size_t log_get_entries_json(const log_filter_t *filter, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) return 0;
    
    log_filter_t f = LOG_FILTER_DEFAULT();
    if (filter) {
        f = *filter;
    }
    
    // Read log file
    int32_t file_size = log_get_file_size();
    if (file_size <= 0) {
        strncpy(buffer, "[]", buffer_size);
        return 2;
    }
    
    // Allocate read buffer
    char *file_buffer = malloc(file_size + 1);
    if (!file_buffer) {
        strncpy(buffer, "[]", buffer_size);
        return 2;
    }
    
    size_t bytes_read = 0;
    if (file_manager_read(LOG_EVENT_FILE, file_buffer, file_size, &bytes_read) != ESP_OK) {
        free(file_buffer);
        strncpy(buffer, "[]", buffer_size);
        return 2;
    }
    file_buffer[bytes_read] = '\0';
    
    // Create JSON array
    cJSON *json_array = cJSON_CreateArray();
    if (!json_array) {
        free(file_buffer);
        strncpy(buffer, "[]", buffer_size);
        return 2;
    }
    
    // Parse lines
    char *line = strtok(file_buffer, "\n");
    uint32_t entry_count = 0;
    uint32_t skipped = 0;
    
    while (line && entry_count < f.max_entries) {
        // Parse log line: timestamp|level|category|source|message
        char timestamp[30] = "";
        char level_str[16] = "";
        char category_str[16] = "";
        char source[32] = "";
        char message[256] = "";
        
        // Simple parsing (for robustness)
        char *p = line;
        char *fields[5] = {NULL};
        int field_idx = 0;
        fields[0] = p;
        
        while (*p && field_idx < 4) {
            if (*p == '|') {
                *p = '\0';
                fields[++field_idx] = p + 1;
            }
            p++;
        }
        
        if (field_idx >= 4) {
            strncpy(timestamp, fields[0] ? fields[0] : "", sizeof(timestamp) - 1);
            strncpy(level_str, fields[1] ? fields[1] : "", sizeof(level_str) - 1);
            strncpy(category_str, fields[2] ? fields[2] : "", sizeof(category_str) - 1);
            strncpy(source, fields[3] ? fields[3] : "", sizeof(source) - 1);
            strncpy(message, fields[4] ? fields[4] : "", sizeof(message) - 1);
            
            // Apply filters
            bool include = true;
            
            // Level filter
            log_level_t level = LOG_LEVEL_INFO;
            for (int i = 0; i < sizeof(s_level_names)/sizeof(s_level_names[0]); i++) {
                if (strcmp(level_str, s_level_names[i]) == 0) {
                    level = (log_level_t)i;
                    break;
                }
            }
            if (level < f.min_level) {
                include = false;
            }
            
            // Category filter
            if (include && f.category >= 0) {
                log_category_t cat = LOG_CAT_SYSTEM;
                for (int i = 0; i < LOG_CAT_MAX; i++) {
                    if (strcmp(category_str, s_category_names[i]) == 0) {
                        cat = (log_category_t)i;
                        break;
                    }
                }
                if (cat != f.category) {
                    include = false;
                }
            }
            
            // Offset filter
            if (include && skipped < f.offset) {
                skipped++;
                include = false;
            }
            
            if (include) {
                cJSON *entry = cJSON_CreateObject();
                cJSON_AddStringToObject(entry, "time", timestamp);
                cJSON_AddStringToObject(entry, "level", level_str);
                cJSON_AddStringToObject(entry, "category", category_str);
                if (source[0]) {
                    cJSON_AddStringToObject(entry, "source", source);
                }
                cJSON_AddStringToObject(entry, "message", message);
                cJSON_AddItemToArray(json_array, entry);
                entry_count++;
            }
        }
        
        line = strtok(NULL, "\n");
    }
    
    free(file_buffer);
    
    // Convert to string
    char *json_str = cJSON_PrintUnformatted(json_array);
    cJSON_Delete(json_array);
    
    if (!json_str) {
        strncpy(buffer, "[]", buffer_size);
        return 2;
    }
    
    size_t len = strlen(json_str);
    if (len >= buffer_size) {
        len = buffer_size - 1;
    }
    memcpy(buffer, json_str, len);
    buffer[len] = '\0';
    free(json_str);
    
    return len;
}

uint32_t log_get_count(void)
{
    int32_t file_size = log_get_file_size();
    if (file_size <= 0) return 0;
    
    char *buffer = malloc(file_size + 1);
    if (!buffer) return 0;
    
    size_t bytes_read = 0;
    if (file_manager_read(LOG_EVENT_FILE, buffer, file_size, &bytes_read) != ESP_OK) {
        free(buffer);
        return 0;
    }
    
    uint32_t count = 0;
    for (size_t i = 0; i < bytes_read; i++) {
        if (buffer[i] == '\n') count++;
    }
    
    free(buffer);
    return count;
}

int32_t log_get_file_size(void)
{
    return file_manager_get_size(LOG_EVENT_FILE);
}

/* ============================================
 * MAINTENANCE
 * ============================================ */
esp_err_t log_rotate(void)
{
    ESP_LOGI(TAG, "Rotating log files...");
    
    // Delete old archive
    file_manager_delete(LOG_ARCHIVE_FILE);
    
    // Rename current to archive
    file_manager_rename(LOG_EVENT_FILE, LOG_ARCHIVE_FILE);
    
    // Create new empty log
    file_manager_write(LOG_EVENT_FILE, "", 0);
    
    log_event(LOG_LEVEL_INFO, LOG_CAT_SYSTEM, NULL, "Log rotated");
    
    return ESP_OK;
}

esp_err_t log_clear(void)
{
    ESP_LOGW(TAG, "Clearing all logs");
    
    file_manager_delete(LOG_ARCHIVE_FILE);
    file_manager_write(LOG_EVENT_FILE, "", 0);
    
    log_event(LOG_LEVEL_WARN, LOG_CAT_SYSTEM, NULL, "Logs cleared");
    
    return ESP_OK;
}

size_t log_export_text(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) return 0;
    
    int32_t file_size = log_get_file_size();
    if (file_size <= 0) {
        buffer[0] = '\0';
        return 0;
    }
    
    size_t to_read = (size_t)file_size;
    if (to_read >= buffer_size) {
        to_read = buffer_size - 1;
    }
    
    size_t bytes_read = 0;
    if (file_manager_read(LOG_EVENT_FILE, buffer, to_read, &bytes_read) != ESP_OK) {
        buffer[0] = '\0';
        return 0;
    }
    
    buffer[bytes_read] = '\0';
    return bytes_read;
}

/* ============================================
 * UTILITIES
 * ============================================ */
const char* log_level_name(log_level_t level)
{
    if (level < sizeof(s_level_names)/sizeof(s_level_names[0])) {
        return s_level_names[level];
    }
    return "UNKNOWN";
}

const char* log_category_name(log_category_t category)
{
    if (category < LOG_CAT_MAX) {
        return s_category_names[category];
    }
    return "UNKNOWN";
}
