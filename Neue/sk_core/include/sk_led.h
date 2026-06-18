#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SK_LED_PATTERN_OFF,
    SK_LED_PATTERN_ON,
    SK_LED_PATTERN_SLOW_BLINK,    // 1 Hz, 50% duty
    SK_LED_PATTERN_FAST_BLINK,    // 5 Hz, 50% duty (pairing mode)
    SK_LED_PATTERN_HEARTBEAT,     // double-pulse per second
} sk_led_pattern_t;

typedef struct {
    int  gpio_num;
    bool active_low;
} sk_led_cfg_t;

esp_err_t sk_led_init(const sk_led_cfg_t *cfg);
void      sk_led_set(sk_led_pattern_t pattern);

#ifdef __cplusplus
}
#endif
