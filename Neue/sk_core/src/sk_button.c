#include "sk_button.h"
#include "sk_event_bus.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sk_button";

#define DEBOUNCE_MS  30

// Adaptive polling. Pre-CONFIG_PM_ENABLE this task spun at a fixed
// 20 ms tick — 50 wakes/sec, harmless on a fully-on CPU. With tickless
// idle + automatic light-sleep, 50 wakes/sec single-handedly defeats
// the LOW_POWER current target (~5 mA average); the CPU never gets a
// sleep window longer than 20 ms.
//
// Strategy: the task subscribes to `power.state` and stretches its
// idle poll period in SCREEN_OFF / LOW_POWER. AFTER an edge is
// detected (any press start or release), it boosts back to ACTIVE
// rate for EDGE_BOOST_MS so multi-tap / hold timing measurements
// stay precise — multi-tap requires sub-100-ms resolution to land
// 5 taps inside an 800 ms window.
#define POLL_PERIOD_ACTIVE_MS    20    // 50 Hz — original behaviour
#define POLL_PERIOD_SCREEN_MS    50    // 20 Hz — display off but recent activity
#define POLL_PERIOD_LOW_MS      100    // 10 Hz — deep idle, light sleep dominant
#define EDGE_BOOST_MS          5000    // stay fast for 5 s after any press edge

static volatile int s_idle_poll_ms = POLL_PERIOD_ACTIVE_MS;
static int64_t      s_last_edge_us = 0;

// Hold thresholds (ms) at which we fire `button.hold` events. Sorted
// ascending, terminator 0. Used by UIs that want progressive visual
// feedback as the user keeps holding the button.
static const int s_hold_thresholds_ms[] = { 1000, 3000, 5000, 10000, 0 };
#define MAX_HOLDS  4

static sk_button_cfg_t  s_cfg;
static sk_button_cb_t   s_cb    = NULL;
static void            *s_user  = NULL;
static bool             s_ready = false;

static int  s_tap_count       = 0;
static int64_t s_first_tap_us = 0;

static bool read_pressed(void)
{
    int lvl = gpio_get_level(s_cfg.gpio_num);
    return s_cfg.active_low ? (lvl == 0) : (lvl != 0);
}

bool sk_button_is_pressed(void) { return s_ready ? read_pressed() : false; }

static void button_task(void *arg)
{
    (void)arg;
    bool    prev_pressed   = false;
    int64_t press_start    = 0;
    bool    long_reported  = false;
    bool    down_reported  = false;
    bool    hold_fired[MAX_HOLDS] = {0};

    for (;;) {
        bool now_pressed = read_pressed();
        int64_t now_us   = esp_timer_get_time();

        if (now_pressed && !prev_pressed) {
            // Edge: released → pressed. Reset all per-press state.
            press_start    = now_us;
            long_reported  = false;
            down_reported  = false;
            for (int i = 0; i < MAX_HOLDS; i++) hold_fired[i] = false;
            s_last_edge_us = now_us;
        }

        if (now_pressed) {
            uint32_t held_ms = (uint32_t)((now_us - press_start) / 1000);

            // button.down — fired ONCE per press, after debounce confirms
            // it's a real press (not a glitch).
            if (!down_reported && held_ms >= DEBOUNCE_MS) {
                down_reported = true;
                sk_event_bus_publish("button.down", NULL);
            }

            // button.hold — fired as each threshold is reached, in order.
            // Lets UIs render progress feedback while the user keeps
            // holding (e.g. growing bar at 1 s, 3 s, 5 s, 10 s marks).
            for (int i = 0; i < MAX_HOLDS && s_hold_thresholds_ms[i] > 0; i++) {
                if (!hold_fired[i] && (int)held_ms >= s_hold_thresholds_ms[i]) {
                    hold_fired[i] = true;
                    sk_event_bus_publishf("button.hold",
                                          "{\"reached_ms\":%d}",
                                          s_hold_thresholds_ms[i]);
                }
            }

            // Long press (legacy, fires once at long_press_ms — kept for
            // backward compatibility with subscribers built before
            // button.hold existed).
            if (!long_reported && held_ms >= s_cfg.long_press_ms) {
                long_reported = true;
                if (s_cb) s_cb(SK_BUTTON_EVT_LONG_PRESS, held_ms, s_user);
                sk_event_bus_publishf("button.long-press",
                                      "{\"duration_ms\":%lu}", (unsigned long)held_ms);
            }
        }

        if (!now_pressed && prev_pressed) {
            // Edge: pressed → released.
            s_last_edge_us = now_us;
            uint32_t held_ms = (uint32_t)((now_us - press_start) / 1000);
            if (held_ms >= DEBOUNCE_MS) {
                sk_event_bus_publishf("button.released",
                                      "{\"duration_ms\":%lu}", (unsigned long)held_ms);

                if (!long_reported) {
                    // Legacy short-press event for short tap subscribers
                    // (existed before button.down/hold).
                    if (s_cb) s_cb(SK_BUTTON_EVT_SHORT_PRESS, held_ms, s_user);
                    sk_event_bus_publishf("button.pressed",
                                          "{\"duration_ms\":%lu}", (unsigned long)held_ms);

                    // Multi-tap window
                    if (s_tap_count == 0 ||
                        (now_us - s_first_tap_us) / 1000 > s_cfg.multi_tap_window_ms) {
                        s_first_tap_us = now_us;
                        s_tap_count = 1;
                    } else {
                        s_tap_count++;
                        if (s_tap_count >= s_cfg.multi_tap_threshold) {
                            if (s_cb) s_cb(SK_BUTTON_EVT_MULTI_TAP, s_tap_count, s_user);
                            sk_event_bus_publishf("button.multi-tap",
                                                  "{\"count\":%d}", s_tap_count);
                            s_tap_count = 0;
                        }
                    }
                }
            }
        }

        prev_pressed = now_pressed;

        // Pick poll period: fast while currently pressed or recently
        // released (multi-tap window protection), idle rate otherwise.
        // Multi-tap requires sub-100-ms timing resolution; the boost
        // ensures we don't miss taps because the user released and
        // light-sleep stretched the poll back out before the next tap.
        int delay_ms;
        if (now_pressed ||
            (now_us - s_last_edge_us) < (int64_t)EDGE_BOOST_MS * 1000) {
            delay_ms = POLL_PERIOD_ACTIVE_MS;
        } else {
            delay_ms = s_idle_poll_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ---------------------------------------------------------------------
// power.state subscriber: throttle the idle poll period so tickless
// light-sleep can hold for the full LOW_POWER window between checks.
// ---------------------------------------------------------------------
static void on_power_state_button(const sk_event_t *evt, void *user)
{
    (void)user;
    if (!evt || !evt->payload_json) return;
    int next;
    if      (strstr(evt->payload_json, "\"low_power\""))  next = POLL_PERIOD_LOW_MS;
    else if (strstr(evt->payload_json, "\"screen_off\"")) next = POLL_PERIOD_SCREEN_MS;
    else                                                  next = POLL_PERIOD_ACTIVE_MS;
    if (next != s_idle_poll_ms) {
        ESP_LOGI(TAG, "idle poll %d → %d ms (power.state)",
                 s_idle_poll_ms, next);
        s_idle_poll_ms = next;
    }
}

esp_err_t sk_button_init(const sk_button_cfg_t *cfg, sk_button_cb_t cb, void *user)
{
    if (s_ready) return ESP_OK;
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    if (s_cfg.long_press_ms == 0)        s_cfg.long_press_ms = 3000;
    if (s_cfg.multi_tap_window_ms == 0)  s_cfg.multi_tap_window_ms = 800;
    if (s_cfg.multi_tap_threshold == 0)  s_cfg.multi_tap_threshold = 5;
    s_cb = cb;
    s_user = user;

    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << s_cfg.gpio_num,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = s_cfg.pullup_internal && s_cfg.active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = s_cfg.pullup_internal && !s_cfg.active_low ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gc);
    if (err != ESP_OK) return err;

    // 6 KB — the sync event chain that fires from this task can cascade
    // through 4-5 subscribers (sk_control → sk_auth → sk_transport_ble),
    // some of which call into NimBLE for advertising refresh. 3 KB was
    // overflowing on long presses → device crashed → bootloop seen as
    // "press-triggered restart".
    BaseType_t ok = xTaskCreate(button_task, "sk_button", 6144, NULL, 4, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    s_ready = true;

    // Subscribe to power.state to throttle the idle poll period in
    // SCREEN_OFF / LOW_POWER. Subscribe failure here is non-fatal —
    // the button keeps working at the default ACTIVE rate, just no
    // power saving on this task.
    int sub;
    esp_err_t serr = sk_event_bus_subscribe("power.state",
                                            on_power_state_button,
                                            NULL, &sub);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "power.state subscribe failed: %s — pinned at %d ms poll",
                 esp_err_to_name(serr), POLL_PERIOD_ACTIVE_MS);
    }

    ESP_LOGI(TAG, "button on GPIO%d (active_%s, poll=%d/%d/%d ms)",
             s_cfg.gpio_num, s_cfg.active_low ? "low" : "high",
             POLL_PERIOD_ACTIVE_MS, POLL_PERIOD_SCREEN_MS, POLL_PERIOD_LOW_MS);
    return ESP_OK;
}
