/**
 * @file timer_scheduler.c
 * @brief Dead Man's Switch Timer Implementation
 */

#include "timer_scheduler.h"
#include "config_manager.h"
#include "time_manager.h"
#include "log_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "timer_sched";

/* ============================================
 * PRIVATE DATA
 * ============================================ */

static timer_state_t s_state = TIMER_STATE_DISABLED;
static timer_config_t s_config;
static timer_runtime_t s_runtime;
static esp_timer_handle_t s_check_timer = NULL;
static timer_trigger_cb_t s_trigger_cb = NULL;
static timer_warning_cb_t s_warning_cb = NULL;

#define CHECK_INTERVAL_MS   (60 * 1000)  // Check every minute

/* ============================================
 * PRIVATE FUNCTIONS
 * ============================================ */

static int64_t get_now_ms(void)
{
    return time_manager_get_timestamp_ms();
}

static bool is_in_time_window(void)
{
    // Empty check times = always active
    if (s_config.check_start[0] == '\0' || s_config.check_end[0] == '\0') {
        return true;
    }
    
    return time_manager_is_in_window(s_config.check_start, s_config.check_end);
}

static void calculate_next_deadline(void)
{
    int64_t now = get_now_ms();
    s_runtime.last_reset = now;
    s_runtime.next_deadline = now + ((int64_t)s_config.interval_minutes * 60 * 1000);
    s_runtime.warnings_sent = 0;
    config_save_runtime(&s_runtime);
    
    ESP_LOGI(TAG, "Next deadline in %lu minutes", (unsigned long)s_config.interval_minutes);
}

static void check_vacation_mode(void)
{
    if (!s_config.vacation_enabled) {
        if (s_state == TIMER_STATE_VACATION) {
            s_state = TIMER_STATE_RUNNING;
            LOG_TIMER(LOG_LEVEL_INFO, "Vacation mode ended");
        }
        return;
    }
    
    int64_t now = get_now_ms();
    int64_t vacation_end = s_config.vacation_start + 
                          ((int64_t)s_config.vacation_days * 24 * 60 * 60 * 1000);
    
    if (now < vacation_end) {
        if (s_state != TIMER_STATE_VACATION) {
            s_state = TIMER_STATE_VACATION;
            LOG_TIMER(LOG_LEVEL_INFO, "Vacation mode active (%lu days)", (unsigned long)s_config.vacation_days);
        }
    } else {
        // Vacation ended
        s_config.vacation_enabled = false;
        config_save_timer(&s_config);
        s_state = TIMER_STATE_RUNNING;
        calculate_next_deadline();
        LOG_TIMER(LOG_LEVEL_INFO, "Vacation mode expired, timer resumed");
    }
}

static void check_warnings(int64_t time_remaining_ms)
{
    if (s_config.warning_minutes <= 0 || s_config.alarm_count <= 0) {
        return;
    }
    
    int64_t warning_threshold_ms = (int64_t)s_config.warning_minutes * 60 * 1000;
    
    if (time_remaining_ms <= warning_threshold_ms && s_runtime.warnings_sent < s_config.alarm_count) {
        // Calculate which warning number
        int warning_num = s_runtime.warnings_sent + 1;
        int mins_remaining = (int)(time_remaining_ms / 60000);
        
        s_runtime.warnings_sent++;
        config_save_runtime(&s_runtime);
        
        s_state = TIMER_STATE_WARNING;
        LOG_TIMER(LOG_LEVEL_WARN, "Warning #%d: %d minutes remaining", warning_num, mins_remaining);
        
        if (s_warning_cb) {
            s_warning_cb(warning_num, mins_remaining);
        }
    }
}

static void do_trigger(void)
{
    s_state = TIMER_STATE_TRIGGERED;
    s_runtime.triggered = true;
    s_runtime.trigger_count++;
    config_save_runtime(&s_runtime);
    
    LOG_TIMER(LOG_LEVEL_CRITICAL, "TIMER TRIGGERED!");
    
    if (s_trigger_cb) {
        s_trigger_cb();
    }
}

static void timer_check_callback(void *arg)
{
    // Skip if disabled or already triggered
    if (s_state == TIMER_STATE_DISABLED || s_runtime.triggered) {
        return;
    }
    
    // Reload config (in case changed)
    config_load_timer(&s_config);
    
    if (!s_config.enabled) {
        s_state = TIMER_STATE_DISABLED;
        return;
    }
    
    // Check vacation mode
    check_vacation_mode();
    if (s_state == TIMER_STATE_VACATION) {
        return;
    }
    
    // Check time window
    if (!is_in_time_window()) {
        if (s_state != TIMER_STATE_PAUSED) {
            s_state = TIMER_STATE_PAUSED;
            ESP_LOGD(TAG, "Timer paused (outside time window)");
        }
        return;
    } else if (s_state == TIMER_STATE_PAUSED) {
        s_state = TIMER_STATE_RUNNING;
        ESP_LOGD(TAG, "Timer resumed (in time window)");
    }
    
    // Calculate remaining time
    int64_t now = get_now_ms();
    int64_t remaining = s_runtime.next_deadline - now;
    
    if (remaining <= 0) {
        // Timer expired - trigger!
        do_trigger();
        return;
    }
    
    // Check for warnings
    check_warnings(remaining);
    
    // Still running
    if (s_state != TIMER_STATE_WARNING) {
        s_state = TIMER_STATE_RUNNING;
    }
}

/* ============================================
 * INITIALIZATION
 * ============================================ */

esp_err_t timer_scheduler_init(void)
{
    ESP_LOGI(TAG, "Initializing timer scheduler...");
    
    // Load config and runtime
    config_load_timer(&s_config);
    config_load_runtime(&s_runtime);
    
    // Check if previously triggered
    if (s_runtime.triggered) {
        s_state = TIMER_STATE_TRIGGERED;
        ESP_LOGW(TAG, "Timer was in triggered state");
    } else if (s_config.enabled) {
        s_state = TIMER_STATE_RUNNING;
        
        // Validate deadline
        int64_t now = get_now_ms();
        if (s_runtime.next_deadline <= 0 || s_runtime.next_deadline < now - 3600000) {
            // No deadline or very old, recalculate
            calculate_next_deadline();
        }
    } else {
        s_state = TIMER_STATE_DISABLED;
    }
    
    // Create periodic timer
    esp_timer_create_args_t timer_args = {
        .callback = timer_check_callback,
        .name = "timer_check"
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &s_check_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_timer_start_periodic(s_check_timer, CHECK_INTERVAL_MS * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Timer scheduler initialized, state=%s", 
             timer_scheduler_state_name(s_state));
    
    return ESP_OK;
}

void timer_scheduler_deinit(void)
{
    if (s_check_timer) {
        esp_timer_stop(s_check_timer);
        esp_timer_delete(s_check_timer);
        s_check_timer = NULL;
    }
}

void timer_scheduler_set_trigger_cb(timer_trigger_cb_t cb)
{
    s_trigger_cb = cb;
}

void timer_scheduler_set_warning_cb(timer_warning_cb_t cb)
{
    s_warning_cb = cb;
}

/* ============================================
 * CONTROL
 * ============================================ */

esp_err_t timer_scheduler_enable(void)
{
    config_load_timer(&s_config);
    s_config.enabled = true;
    config_save_timer(&s_config);
    
    s_runtime.triggered = false;
    calculate_next_deadline();
    
    s_state = TIMER_STATE_RUNNING;
    LOG_TIMER(LOG_LEVEL_INFO, "Timer enabled");
    
    return ESP_OK;
}

esp_err_t timer_scheduler_disable(void)
{
    config_load_timer(&s_config);
    s_config.enabled = false;
    config_save_timer(&s_config);
    
    s_state = TIMER_STATE_DISABLED;
    LOG_TIMER(LOG_LEVEL_INFO, "Timer disabled");
    
    return ESP_OK;
}

esp_err_t timer_scheduler_reset(void)
{
    if (s_state == TIMER_STATE_DISABLED) {
        ESP_LOGW(TAG, "Cannot reset - timer disabled");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_runtime.triggered) {
        ESP_LOGW(TAG, "Timer already triggered, use acknowledge first");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_runtime.reset_count++;
    calculate_next_deadline();
    s_state = TIMER_STATE_RUNNING;
    
    LOG_TIMER(LOG_LEVEL_INFO, "Timer reset (count: %lu)", s_runtime.reset_count);
    
    return ESP_OK;
}

esp_err_t timer_scheduler_acknowledge(void)
{
    if (!s_runtime.triggered) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_runtime.triggered = false;
    calculate_next_deadline();
    s_state = TIMER_STATE_RUNNING;
    
    LOG_TIMER(LOG_LEVEL_INFO, "Trigger acknowledged, timer restarted");
    
    return ESP_OK;
}

/* ============================================
 * VACATION MODE
 * ============================================ */

esp_err_t timer_scheduler_vacation_start(int days)
{
    if (days <= 0 || days > 365) {
        return ESP_ERR_INVALID_ARG;
    }
    
    config_load_timer(&s_config);
    s_config.vacation_enabled = true;
    s_config.vacation_days = days;
    s_config.vacation_start = get_now_ms();
    config_save_timer(&s_config);
    
    s_state = TIMER_STATE_VACATION;
    LOG_TIMER(LOG_LEVEL_INFO, "Vacation mode started for %d days", days);
    
    return ESP_OK;
}

esp_err_t timer_scheduler_vacation_end(void)
{
    config_load_timer(&s_config);
    s_config.vacation_enabled = false;
    config_save_timer(&s_config);
    
    if (s_config.enabled) {
        s_state = TIMER_STATE_RUNNING;
        calculate_next_deadline();
    } else {
        s_state = TIMER_STATE_DISABLED;
    }
    
    LOG_TIMER(LOG_LEVEL_INFO, "Vacation mode ended");
    
    return ESP_OK;
}

bool timer_scheduler_is_vacation(void)
{
    return s_state == TIMER_STATE_VACATION;
}

/* ============================================
 * STATUS
 * ============================================ */

void timer_scheduler_get_status(timer_status_t *status)
{
    if (!status) return;
    
    status->state = s_state;
    status->next_deadline = s_runtime.next_deadline;
    status->warnings_sent = s_runtime.warnings_sent;
    status->reset_count = s_runtime.reset_count;
    status->trigger_count = s_runtime.trigger_count;
    status->in_time_window = is_in_time_window();
    
    int64_t now = get_now_ms();
    status->time_remaining_ms = s_runtime.next_deadline - now;
    if (status->time_remaining_ms < 0) {
        status->time_remaining_ms = 0;
    }
}

timer_state_t timer_scheduler_get_state(void)
{
    return s_state;
}

bool timer_scheduler_is_triggered(void)
{
    return s_runtime.triggered;
}

int64_t timer_scheduler_time_remaining_ms(void)
{
    int64_t now = get_now_ms();
    int64_t remaining = s_runtime.next_deadline - now;
    return (remaining > 0) ? remaining : 0;
}

void timer_scheduler_time_remaining_str(char *buffer, size_t size)
{
    if (!buffer || size == 0) return;
    
    int64_t remaining_ms = timer_scheduler_time_remaining_ms();
    int64_t remaining_sec = remaining_ms / 1000;
    
    int hours = remaining_sec / 3600;
    int mins = (remaining_sec % 3600) / 60;
    
    if (s_state == TIMER_STATE_DISABLED) {
        snprintf(buffer, size, "Disabled");
    } else if (s_state == TIMER_STATE_TRIGGERED) {
        snprintf(buffer, size, "TRIGGERED");
    } else if (s_state == TIMER_STATE_VACATION) {
        snprintf(buffer, size, "Vacation");
    } else if (hours > 0) {
        snprintf(buffer, size, "%dh %dm", hours, mins);
    } else if (mins > 0) {
        snprintf(buffer, size, "%dm", mins);
    } else {
        snprintf(buffer, size, "< 1m");
    }
}

const char* timer_scheduler_state_name(timer_state_t state)
{
    switch (state) {
        case TIMER_STATE_DISABLED:  return "DISABLED";
        case TIMER_STATE_RUNNING:   return "RUNNING";
        case TIMER_STATE_WARNING:   return "WARNING";
        case TIMER_STATE_TRIGGERED: return "TRIGGERED";
        case TIMER_STATE_PAUSED:    return "PAUSED";
        case TIMER_STATE_VACATION:  return "VACATION";
        default:                    return "UNKNOWN";
    }
}
