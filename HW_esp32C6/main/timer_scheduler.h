/**
 * @file timer_scheduler.h
 * @brief Dead Man's Switch Timer Scheduler
 * 
 * Core logic for the dead man's switch:
 * - Countdown timer with configurable interval
 * - Warning notifications before trigger
 * - Time window checks (quiet hours)
 * - Vacation mode
 * - Reset mechanism
 */

#ifndef TIMER_SCHEDULER_H
#define TIMER_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * TIMER STATES
 * ============================================ */
typedef enum {
    TIMER_STATE_DISABLED,       // Timer not active
    TIMER_STATE_RUNNING,        // Normal countdown
    TIMER_STATE_WARNING,        // Warning period (before trigger)
    TIMER_STATE_TRIGGERED,      // Timer has triggered
    TIMER_STATE_PAUSED,         // Paused (outside time window)
    TIMER_STATE_VACATION        // Vacation mode
} timer_state_t;

/* ============================================
 * TIMER STATUS
 * ============================================ */
typedef struct {
    timer_state_t state;
    int64_t next_deadline;      // Unix timestamp (ms)
    int64_t time_remaining_ms;  // Milliseconds until deadline
    int warnings_sent;          // Number of warnings sent
    int reset_count;            // Total resets
    int trigger_count;          // Total triggers
    bool in_time_window;        // Currently in active window
} timer_status_t;

/* ============================================
 * CALLBACKS
 * ============================================ */

/**
 * @brief Callback when timer triggers (deadline reached)
 */
typedef void (*timer_trigger_cb_t)(void);

/**
 * @brief Callback when warning should be sent
 * @param warning_number Which warning (1, 2, 3...)
 * @param minutes_remaining Minutes until trigger
 */
typedef void (*timer_warning_cb_t)(int warning_number, int minutes_remaining);

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize timer scheduler
 */
esp_err_t timer_scheduler_init(void);

/**
 * @brief Deinitialize timer scheduler
 */
void timer_scheduler_deinit(void);

/**
 * @brief Set trigger callback
 */
void timer_scheduler_set_trigger_cb(timer_trigger_cb_t cb);

/**
 * @brief Set warning callback
 */
void timer_scheduler_set_warning_cb(timer_warning_cb_t cb);

/* ============================================
 * CONTROL
 * ============================================ */

/**
 * @brief Enable/Start the timer
 */
esp_err_t timer_scheduler_enable(void);

/**
 * @brief Disable/Stop the timer
 */
esp_err_t timer_scheduler_disable(void);

/**
 * @brief Reset the timer ("I'm alive" signal)
 * @return ESP_OK on success
 */
esp_err_t timer_scheduler_reset(void);

/**
 * @brief Acknowledge trigger (stop alarm)
 */
esp_err_t timer_scheduler_acknowledge(void);

/* ============================================
 * VACATION MODE
 * ============================================ */

/**
 * @brief Enable vacation mode
 * @param days Number of vacation days
 */
esp_err_t timer_scheduler_vacation_start(int days);

/**
 * @brief Disable vacation mode
 */
esp_err_t timer_scheduler_vacation_end(void);

/**
 * @brief Check if in vacation mode
 */
bool timer_scheduler_is_vacation(void);

/* ============================================
 * STATUS
 * ============================================ */

/**
 * @brief Get current timer status
 * @param status Output status structure
 */
void timer_scheduler_get_status(timer_status_t *status);

/**
 * @brief Get timer state
 */
timer_state_t timer_scheduler_get_state(void);

/**
 * @brief Check if timer has triggered
 */
bool timer_scheduler_is_triggered(void);

/**
 * @brief Get time remaining in milliseconds
 */
int64_t timer_scheduler_time_remaining_ms(void);

/**
 * @brief Get time remaining as string (e.g., "2h 30m")
 */
void timer_scheduler_time_remaining_str(char *buffer, size_t size);

/* ============================================
 * STATE NAME
 * ============================================ */

/**
 * @brief Get state name string
 */
const char* timer_scheduler_state_name(timer_state_t state);

#ifdef __cplusplus
}
#endif

#endif // TIMER_SCHEDULER_H
