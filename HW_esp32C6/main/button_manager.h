/**
 * @file button_manager.h
 * @brief Button Input Manager
 * 
 * GPIO17 = Button (Pull-up, active LOW)
 * 
 * Supports:
 * - Short press detection
 * - Long press detection
 * - Multi-click detection
 */

#ifndef BUTTON_MANAGER_H
#define BUTTON_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * CONFIGURATION
 * ============================================ */
#define BUTTON_GPIO             17      // Button pin
#define BUTTON_DEBOUNCE_MS      50      // Debounce time
#define BUTTON_LONG_PRESS_MS    3000    // Long press threshold
#define BUTTON_MULTI_CLICK_MS   500     // Max time between clicks

/* ============================================
 * BUTTON EVENTS
 * ============================================ */
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESSED,       // Button pressed (not released)
    BUTTON_EVENT_RELEASED,      // Button released
    BUTTON_EVENT_CLICK,         // Single short click
    BUTTON_EVENT_DOUBLE_CLICK,  // Double click
    BUTTON_EVENT_TRIPLE_CLICK,  // Triple click
    BUTTON_EVENT_LONG_PRESS,    // Long press started
    BUTTON_EVENT_LONG_RELEASE   // Released after long press
} button_event_t;

/* ============================================
 * CALLBACKS
 * ============================================ */

/**
 * @brief Button event callback
 * @param event Event type
 */
typedef void (*button_event_cb_t)(button_event_t event);

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize button manager
 */
esp_err_t button_manager_init(void);

/**
 * @brief Deinitialize button manager
 */
void button_manager_deinit(void);

/**
 * @brief Set event callback
 */
void button_manager_set_callback(button_event_cb_t cb);

/* ============================================
 * STATUS
 * ============================================ */

/**
 * @brief Check if button is currently pressed
 */
bool button_is_pressed(void);

/**
 * @brief Get last event
 */
button_event_t button_get_last_event(void);

/**
 * @brief Get press duration in milliseconds
 * @return Duration if pressed, 0 if not pressed
 */
uint32_t button_get_press_duration(void);

/**
 * @brief Get event name string
 */
const char* button_event_name(button_event_t event);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_MANAGER_H
