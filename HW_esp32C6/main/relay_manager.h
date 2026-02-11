/**
 * @file relay_manager.h
 * @brief Relay Control Manager
 * 
 * GPIO18 = Relay (Active HIGH)
 */

#ifndef RELAY_MANAGER_H
#define RELAY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * CONFIGURATION
 * ============================================ */
#define RELAY_GPIO          18      // Relay control pin
#define RELAY_ACTIVE_HIGH   true    // Active high = ON when GPIO high

/* ============================================
 * RELAY STATE
 * ============================================ */
typedef enum {
    RELAY_OFF = 0,
    RELAY_ON = 1
} relay_state_t;

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize relay manager
 */
esp_err_t relay_manager_init(void);

/**
 * @brief Deinitialize relay manager
 */
void relay_manager_deinit(void);

/* ============================================
 * CONTROL
 * ============================================ */

/**
 * @brief Turn relay ON
 */
esp_err_t relay_on(void);

/**
 * @brief Turn relay OFF
 */
esp_err_t relay_off(void);

/**
 * @brief Toggle relay state
 */
esp_err_t relay_toggle(void);

/**
 * @brief Set relay state
 */
esp_err_t relay_set(relay_state_t state);

/**
 * @brief Pulse relay (ON, wait, OFF)
 * @param duration_ms Pulse duration in milliseconds
 */
esp_err_t relay_pulse(uint32_t duration_ms);

/**
 * @brief Multiple pulses
 * @param count Number of pulses
 * @param on_ms ON duration
 * @param off_ms OFF duration between pulses
 */
esp_err_t relay_pulse_sequence(int count, uint32_t on_ms, uint32_t off_ms);

/* ============================================
 * STATUS
 * ============================================ */

/**
 * @brief Get current relay state
 */
relay_state_t relay_get_state(void);

/**
 * @brief Check if relay is ON
 */
bool relay_is_on(void);

/**
 * @brief Get relay on duration (in ms since turned on)
 * @return Duration in ms, 0 if off
 */
uint32_t relay_get_on_duration(void);

#ifdef __cplusplus
}
#endif

#endif // RELAY_MANAGER_H
