#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// ls_relay — LebensSpur relay output controller
//
// A single GPIO output (default GPIO 19, may change until the LS PCB
// rev-A pin map is finalized). User settings are stored in NVS.
// Behavior:
//
//   When FIRE is triggered:
//     1. Wait start_delay_sec (0-10 s)
//     2. If pulse mode is OFF:
//        - GPIO ON, wait total_duration, GPIO OFF
//     3. If pulse mode is ON:
//        - cycle = (pulse_duration_sec s ON) + (pulse_duration_sec s OFF)
//        - total cycle count = total_duration / (2 * pulse_duration_sec)
//        - each cycle: ON -> wait -> OFF -> wait
//        - if cycle count is 0, run at least 1 cycle
//     4. Return GPIO to idle state (depending on invert flag)
//
// Invert: false -> idle LOW + active HIGH (default).
//         true  -> idle HIGH + active LOW.
//
// Event subscriptions:
//   timer.triggered -> automatic FIRE (engine's normal flow)
//
// Event publications:
//   relay.fire.start  {"reason":"timer"|"manual"}
//   relay.fire.end    {"ok":true,"aborted":false}
//
// CLI commands (each changes a single property, space-separated UX):
//   relay gpio <N>           : output pin (0..30)
//   relay invert <on|off>    : invert polarity
//   relay delay <s>          : start delay (0..10)
//   relay duration <s>       : total active duration (1..3600)
//   relay pulse <on|off>     : enable/disable pulse mode
//   relay pulse <s>          : enable pulse mode AND set half-cycle (1..60)
//   relay get                : show configuration
//   relay test               : manual FIRE (full sequence per config)
//   relay fire               : alias of relay test (hidden)
//   relay abort              : cancel in-progress FIRE
//   relay off                : force relay idle (kill switch)
//   relay status             : runtime state
// =====================================================================

#define LS_RELAY_MIN_DURATION_SEC      1u
#define LS_RELAY_MAX_DURATION_SEC      3600u    // 60-minute cap
#define LS_RELAY_MAX_DELAY_SEC         10u
#define LS_RELAY_MIN_PULSE_SEC         1u
#define LS_RELAY_MAX_PULSE_SEC         60u

typedef struct {
    int      gpio_num;            // output pin
    bool     invert;              // false: idle LOW, active HIGH; true: reversed
    uint16_t start_delay_sec;     // 0..10
    uint32_t total_duration_sec;  // 1..3600
    bool     pulse_enabled;
    uint16_t pulse_duration_sec;  // 1..60 (half-cycle duration: time ON, time OFF)
} ls_relay_config_t;

typedef enum {
    LS_RELAY_IDLE = 0,
    LS_RELAY_DELAYING,
    LS_RELAY_ACTIVE,
    LS_RELAY_PULSING,
} ls_relay_phase_t;

typedef struct {
    ls_relay_phase_t phase;
    bool             gpio_level;       // current output (driver-visible level)
    bool             gpio_active;      // logical "active" (independent of invert)
    int64_t          fire_started_us;  // 0 = inactive
} ls_relay_status_t;

esp_err_t ls_relay_init(int default_gpio_num);

void ls_relay_status(ls_relay_status_t *out);
void ls_relay_config(ls_relay_config_t *out);

#ifdef __cplusplus
}
#endif
