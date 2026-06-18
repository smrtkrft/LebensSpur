#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      gpio_num;
    bool     active_low;              // true if button pulls pin to GND when pressed
    bool     pullup_internal;         // enable internal pull-up/-down
    uint32_t long_press_ms;           // default 3000
    uint32_t multi_tap_window_ms;     // default 800
    uint8_t  multi_tap_threshold;     // number of taps to trigger factory reset (default 5)
} sk_button_cfg_t;

typedef enum {
    SK_BUTTON_EVT_SHORT_PRESS,
    SK_BUTTON_EVT_LONG_PRESS,
    SK_BUTTON_EVT_MULTI_TAP,
} sk_button_event_t;

typedef void (*sk_button_cb_t)(sk_button_event_t evt, uint32_t meta, void *user);

esp_err_t sk_button_init(const sk_button_cfg_t *cfg, sk_button_cb_t cb, void *user);

// Returns true if the button is currently physically pressed.
bool sk_button_is_pressed(void);

#ifdef __cplusplus
}
#endif
