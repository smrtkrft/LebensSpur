/**
 * @file time_manager.c
 * @brief Time Manager Implementation
 */

#include "time_manager.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "time_mgr";

/* ============================================
 * PRIVATE DATA
 * ============================================ */
static bool s_time_synced = false;
static int64_t s_boot_time_us = 0;
static char s_timezone[64] = DEFAULT_TIMEZONE;

/* ============================================
 * SNTP CALLBACK
 * ============================================ */
static void sntp_sync_callback(struct timeval *tv)
{
    s_time_synced = true;
    ESP_LOGI(TAG, "NTP sync complete: %lld", (long long)tv->tv_sec);
}

/* ============================================
 * INITIALIZATION
 * ============================================ */
esp_err_t time_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing time manager...");
    
    // Record boot time
    s_boot_time_us = esp_timer_get_time();
    
    // Set timezone
    setenv("TZ", s_timezone, 1);
    tzset();
    
    ESP_LOGI(TAG, "Time manager initialized, TZ=%s", s_timezone);
    return ESP_OK;
}

void time_manager_deinit(void)
{
    time_manager_stop_ntp();
}

/* ============================================
 * NTP SYNCHRONIZATION
 * ============================================ */
esp_err_t time_manager_start_ntp(void)
{
    ESP_LOGI(TAG, "Starting NTP sync...");
    
    // Stop if already running
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    
    s_time_synced = false;
    
    // Configure SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_PRIMARY);
    esp_sntp_setservername(1, NTP_SERVER_SECONDARY);
    esp_sntp_setservername(2, NTP_SERVER_TERTIARY);
    
    sntp_set_time_sync_notification_cb(sntp_sync_callback);
    sntp_set_sync_interval(NTP_SYNC_INTERVAL_S * 1000);
    
    esp_sntp_init();
    
    ESP_LOGI(TAG, "NTP servers: %s, %s, %s", 
             NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY, NTP_SERVER_TERTIARY);
    
    return ESP_OK;
}

void time_manager_stop_ntp(void)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        ESP_LOGI(TAG, "NTP stopped");
    }
}

esp_err_t time_manager_sync_now(void)
{
    s_time_synced = false;
    
    if (esp_sntp_enabled()) {
        esp_sntp_restart();
    } else {
        time_manager_start_ntp();
    }
    
    return ESP_OK;
}

bool time_manager_is_synced(void)
{
    return s_time_synced;
}

bool time_manager_wait_sync(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    
    while (!s_time_synced) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            ESP_LOGW(TAG, "NTP sync timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    return true;
}

/* ============================================
 * TIMEZONE
 * ============================================ */
void time_manager_set_timezone(const char *tz)
{
    if (!tz) return;
    
    strncpy(s_timezone, tz, sizeof(s_timezone) - 1);
    s_timezone[sizeof(s_timezone) - 1] = '\0';
    
    setenv("TZ", s_timezone, 1);
    tzset();
    
    ESP_LOGI(TAG, "Timezone set: %s", s_timezone);
}

const char* time_manager_get_timezone(void)
{
    return s_timezone;
}

/* ============================================
 * TIME FUNCTIONS
 * ============================================ */
time_t time_manager_get_timestamp(void)
{
    time_t now;
    time(&now);
    return now;
}

int64_t time_manager_get_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void tm_to_time_info(const struct tm *tm, time_info_t *info)
{
    info->year = tm->tm_year + 1900;
    info->month = tm->tm_mon + 1;
    info->day = tm->tm_mday;
    info->hour = tm->tm_hour;
    info->minute = tm->tm_min;
    info->second = tm->tm_sec;
    info->weekday = tm->tm_wday;
}

void time_manager_get_local(time_info_t *info)
{
    if (!info) return;
    
    time_t now = time_manager_get_timestamp();
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    tm_to_time_info(&tm_local, info);
}

void time_manager_get_utc(time_info_t *info)
{
    if (!info) return;
    
    time_t now = time_manager_get_timestamp();
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    tm_to_time_info(&tm_utc, info);
}

void time_manager_get_string(char *buffer, size_t size, const char *format)
{
    if (!buffer || size == 0) return;
    
    time_t now = time_manager_get_timestamp();
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    
    const char *fmt = format ? format : "%Y-%m-%d %H:%M:%S";
    strftime(buffer, size, fmt, &tm_local);
}

void time_manager_get_iso8601(char *buffer, size_t size)
{
    if (!buffer || size < 25) return;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    struct tm tm_utc;
    gmtime_r(&tv.tv_sec, &tm_utc);
    
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &tm_utc);
    
    // Add milliseconds and Z
    int ms = tv.tv_usec / 1000;
    snprintf(buffer + strlen(buffer), size - strlen(buffer), ".%03dZ", ms);
}

/* ============================================
 * UPTIME
 * ============================================ */
void time_manager_get_uptime(uptime_t *uptime)
{
    if (!uptime) return;
    
    uint32_t total_seconds = time_manager_get_uptime_seconds();
    
    uptime->days = total_seconds / 86400;
    total_seconds %= 86400;
    
    uptime->hours = total_seconds / 3600;
    total_seconds %= 3600;
    
    uptime->minutes = total_seconds / 60;
    uptime->seconds = total_seconds % 60;
}

uint32_t time_manager_get_uptime_seconds(void)
{
    int64_t uptime_us = esp_timer_get_time() - s_boot_time_us;
    return (uint32_t)(uptime_us / 1000000);
}

void time_manager_get_uptime_string(char *buffer, size_t size)
{
    if (!buffer || size == 0) return;
    
    uptime_t uptime;
    time_manager_get_uptime(&uptime);
    
    if (uptime.days > 0) {
        snprintf(buffer, size, "%lud %uh %um %us", 
                 uptime.days, uptime.hours, uptime.minutes, uptime.seconds);
    } else if (uptime.hours > 0) {
        snprintf(buffer, size, "%uh %um %us", 
                 uptime.hours, uptime.minutes, uptime.seconds);
    } else if (uptime.minutes > 0) {
        snprintf(buffer, size, "%um %us", uptime.minutes, uptime.seconds);
    } else {
        snprintf(buffer, size, "%us", uptime.seconds);
    }
}

/* ============================================
 * UTILITIES
 * ============================================ */
esp_err_t time_manager_parse_hhmm(const char *hhmm, int *hours, int *minutes)
{
    if (!hhmm || !hours || !minutes) return ESP_ERR_INVALID_ARG;
    
    int h, m;
    if (sscanf(hhmm, "%d:%d", &h, &m) != 2) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *hours = h;
    *minutes = m;
    return ESP_OK;
}

bool time_manager_is_in_window(const char *start_hhmm, const char *end_hhmm)
{
    int start_h, start_m, end_h, end_m;
    
    if (time_manager_parse_hhmm(start_hhmm, &start_h, &start_m) != ESP_OK) {
        return true;  // Invalid start = always in window
    }
    if (time_manager_parse_hhmm(end_hhmm, &end_h, &end_m) != ESP_OK) {
        return true;  // Invalid end = always in window
    }
    
    time_info_t now;
    time_manager_get_local(&now);
    
    int now_minutes = now.hour * 60 + now.minute;
    int start_minutes = start_h * 60 + start_m;
    int end_minutes = end_h * 60 + end_m;
    
    // Handle overnight window (e.g., 22:00 - 06:00)
    if (start_minutes > end_minutes) {
        return (now_minutes >= start_minutes || now_minutes <= end_minutes);
    }
    
    return (now_minutes >= start_minutes && now_minutes <= end_minutes);
}

const char* time_manager_get_day_name(int weekday)
{
    static const char *days[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", 
        "Thursday", "Friday", "Saturday"
    };
    
    if (weekday >= 0 && weekday <= 6) {
        return days[weekday];
    }
    return "Unknown";
}
