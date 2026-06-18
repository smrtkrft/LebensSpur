// =====================================================================
// ls_relay — implementation. See header.
//
// The worker task is always alive in the background; it receives
// FIRE / ABORT events via the queue. During FIRE, every sleep loop
// checks the abort flag; if abort arrives, the routine cleanly
// returns to IDLE and emits the relay.fire.end event with
// {"ok":false,"aborted":true}.
// =====================================================================

#include "ls_relay.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"

static const char *TAG = "ls_relay";

#define NVS_NS                "ls_rly"
#define NVS_KEY_GPIO          "gpio"
#define NVS_KEY_INVERT        "invert"
#define NVS_KEY_DELAY         "delay"
#define NVS_KEY_DURATION      "dur"
#define NVS_KEY_PULSE_EN      "pulse_en"
#define NVS_KEY_PULSE_DUR     "pulse_d"

#define DEFAULT_DELAY_SEC     0
#define DEFAULT_DURATION_SEC  60
#define DEFAULT_PULSE_DUR_SEC 5

// ---------------------------------------------------------------------
// State
// ---------------------------------------------------------------------

typedef enum {
    EVT_FIRE,
    EVT_ABORT,
} evt_type_t;

typedef enum {
    FIRE_REASON_TIMER,
    FIRE_REASON_MANUAL,
} fire_reason_t;

typedef struct {
    evt_type_t    type;
    fire_reason_t reason;
} evt_t;

static QueueHandle_t s_q = NULL;
// Default GPIO captured at init (passed by main as LS_RELAY_DEFAULT_GPIO).
// Stashed so the factory-reset handler can restore the board's canonical
// pin without depending on a constant from main.c.
static int s_default_gpio = -1;
static ls_relay_config_t s_cfg = {
    .gpio_num           = -1,
    .invert             = false,
    .start_delay_sec    = DEFAULT_DELAY_SEC,
    .total_duration_sec = DEFAULT_DURATION_SEC,
    .pulse_enabled      = false,
    .pulse_duration_sec = DEFAULT_PULSE_DUR_SEC,
};
static ls_relay_phase_t s_phase = LS_RELAY_IDLE;
static int64_t s_fire_started_us = 0;
static atomic_bool s_abort = ATOMIC_VAR_INIT(false);
// Cached logical "active" state. gpio_get_level() on an OUTPUT-only pin can
// be unreliable on some chips, so we track the driven state ourselves.
static atomic_bool s_active = ATOMIC_VAR_INIT(false);

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static const char *phase_str(ls_relay_phase_t p)
{
    switch (p) {
    case LS_RELAY_IDLE:     return "idle";
    case LS_RELAY_DELAYING: return "delaying";
    case LS_RELAY_ACTIVE:   return "active";
    case LS_RELAY_PULSING:  return "pulsing";
    default:                return "?";
    }
}

static const char *reason_str(fire_reason_t r)
{
    return (r == FIRE_REASON_MANUAL) ? "manual" : "timer";
}

static int idle_level(void)  { return s_cfg.invert ? 1 : 0; }
static int active_level(void){ return s_cfg.invert ? 0 : 1; }

static void gpio_write_idle(void)
{
    if (s_cfg.gpio_num >= 0) gpio_set_level(s_cfg.gpio_num, idle_level());
    atomic_store(&s_active, false);
}

static void gpio_write_active(void)
{
    if (s_cfg.gpio_num >= 0) gpio_set_level(s_cfg.gpio_num, active_level());
    atomic_store(&s_active, true);
}

// Sleep ms but check the abort flag every 100 ms. Returns true if
// aborted early, false if the full duration completed.
static bool cancellable_sleep_ms(uint32_t total_ms)
{
    const uint32_t chunk = 100;
    uint32_t left = total_ms;
    while (left > 0) {
        if (atomic_load(&s_abort)) return true;
        uint32_t step = (left < chunk) ? left : chunk;
        vTaskDelay(pdMS_TO_TICKS(step));
        left -= step;
    }
    return atomic_load(&s_abort);
}

// ---------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------

static void nvs_load(int default_gpio_num)
{
    s_cfg.gpio_num = default_gpio_num;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) return;

    int32_t i32;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;

    if (nvs_get_i32(h, NVS_KEY_GPIO, &i32) == ESP_OK)        s_cfg.gpio_num = (int)i32;
    if (nvs_get_u8(h, NVS_KEY_INVERT, &u8) == ESP_OK)        s_cfg.invert = (u8 != 0);
    if (nvs_get_u16(h, NVS_KEY_DELAY, &u16) == ESP_OK)       s_cfg.start_delay_sec = u16;
    if (nvs_get_u32(h, NVS_KEY_DURATION, &u32) == ESP_OK)    s_cfg.total_duration_sec = u32;
    if (nvs_get_u8(h, NVS_KEY_PULSE_EN, &u8) == ESP_OK)      s_cfg.pulse_enabled = (u8 != 0);
    if (nvs_get_u16(h, NVS_KEY_PULSE_DUR, &u16) == ESP_OK)   s_cfg.pulse_duration_sec = u16;

    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_GPIO,      (int32_t)s_cfg.gpio_num);
    nvs_set_u8(h,  NVS_KEY_INVERT,    s_cfg.invert ? 1 : 0);
    nvs_set_u16(h, NVS_KEY_DELAY,     s_cfg.start_delay_sec);
    nvs_set_u32(h, NVS_KEY_DURATION,  s_cfg.total_duration_sec);
    nvs_set_u8(h,  NVS_KEY_PULSE_EN,  s_cfg.pulse_enabled ? 1 : 0);
    nvs_set_u16(h, NVS_KEY_PULSE_DUR, s_cfg.pulse_duration_sec);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------
// Factory reset hook
// ---------------------------------------------------------------------

// On device.factory-reset.requested: drive GPIO to idle immediately
// (load must not stay energized), abort any in-progress fire, wipe NVS,
// reset cfg to defaults. Pairing-completeness fix: previously the relay
// GPIO/polarity/duration/pulse persisted, so a new owner could inherit
// a relay wired to the previous owner's load.
static void on_factory_reset(const sk_event_t *evt, void *user_ctx)
{
    (void)evt; (void)user_ctx;
    ESP_LOGW(TAG, "factory reset received — wiping ls_relay NVS + state");

    // 1) Safety FIRST: drive current GPIO line to idle level (using the
    // current polarity, before we reset invert) so the load is not left
    // energized while we tear cfg down.
    if (s_cfg.gpio_num >= 0) {
        gpio_set_level(s_cfg.gpio_num, s_cfg.invert ? 1 : 0);
    }
    atomic_store(&s_active, false);

    // 2) Abort any in-progress fire: the worker's cancellable_sleep_ms
    // will pick up s_abort and tear down cleanly. Also force phase to
    // IDLE in case worker is between sleeps.
    atomic_store(&s_abort, true);
    s_phase = LS_RELAY_IDLE;
    s_fire_started_us = 0;

    // 3) Reset config to defaults.
    s_cfg.gpio_num           = s_default_gpio;
    s_cfg.invert             = false;
    s_cfg.start_delay_sec    = DEFAULT_DELAY_SEC;
    s_cfg.total_duration_sec = DEFAULT_DURATION_SEC;
    s_cfg.pulse_enabled      = false;
    s_cfg.pulse_duration_sec = DEFAULT_PULSE_DUR_SEC;

    // 4) Wipe NVS namespace.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    // 5) Re-assert idle level on the (possibly restored-default) GPIO.
    if (s_cfg.gpio_num >= 0) {
        gpio_reset_pin(s_cfg.gpio_num);
        gpio_set_direction(s_cfg.gpio_num, GPIO_MODE_OUTPUT);
        gpio_write_idle();
    }
}

// ---------------------------------------------------------------------
// Fire sequence (executed in worker task)
// ---------------------------------------------------------------------

static void run_fire_sequence(fire_reason_t reason)
{
    atomic_store(&s_abort, false);
    s_fire_started_us = esp_timer_get_time();

    sk_event_bus_publishf("relay.fire.start",
        "{\"reason\":\"%s\"}", reason_str(reason));

    // 1) Start delay
    if (s_cfg.start_delay_sec > 0) {
        s_phase = LS_RELAY_DELAYING;
        if (cancellable_sleep_ms(s_cfg.start_delay_sec * 1000u)) goto aborted;
    }

    if (s_cfg.pulse_enabled && s_cfg.pulse_duration_sec > 0) {
        // 2) Pulse mode
        s_phase = LS_RELAY_PULSING;
        uint32_t cycle_ms = s_cfg.pulse_duration_sec * 2u * 1000u;
        uint32_t total_ms = s_cfg.total_duration_sec * 1000u;
        uint32_t cycles = (cycle_ms > 0) ? (total_ms / cycle_ms) : 1u;
        if (cycles == 0) cycles = 1;
        for (uint32_t i = 0; i < cycles; ++i) {
            gpio_write_active();
            if (cancellable_sleep_ms(s_cfg.pulse_duration_sec * 1000u)) goto aborted;
            gpio_write_idle();
            if (cancellable_sleep_ms(s_cfg.pulse_duration_sec * 1000u)) goto aborted;
        }
    } else {
        // 2) Steady ON for total_duration
        s_phase = LS_RELAY_ACTIVE;
        gpio_write_active();
        if (cancellable_sleep_ms(s_cfg.total_duration_sec * 1000u)) goto aborted;
        gpio_write_idle();
    }

    s_phase = LS_RELAY_IDLE;
    s_fire_started_us = 0;
    sk_event_bus_publish("relay.fire.end", "{\"ok\":true,\"aborted\":false}");
    return;

aborted:
    gpio_write_idle();
    s_phase = LS_RELAY_IDLE;
    s_fire_started_us = 0;
    sk_event_bus_publish("relay.fire.end", "{\"ok\":false,\"aborted\":true}");
}

static void worker_task(void *arg)
{
    (void)arg;
    evt_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        if (e.type == EVT_FIRE) {
            if (s_phase != LS_RELAY_IDLE) {
                ESP_LOGW(TAG, "fire ignored: already running (phase=%s)", phase_str(s_phase));
                continue;
            }
            run_fire_sequence(e.reason);
        } else if (e.type == EVT_ABORT) {
            // Abort is normally signalled by writing s_abort directly (the
            // worker is blocked in run_fire_sequence's cancellable_sleep_ms
            // and only the atomic flag can wake it). This queue branch only
            // executes when the worker is already IDLE — in that case there
            // is nothing to abort; just clear any stale flag.
            atomic_store(&s_abort, false);
        }
    }
}

// ---------------------------------------------------------------------
// Event subscriber
// ---------------------------------------------------------------------

static void on_timer_triggered(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    // Ignore timer-driven fires while already firing — otherwise a second
    // fire would queue up and run back-to-back when the current one ends.
    if (s_phase != LS_RELAY_IDLE) {
        ESP_LOGW(TAG, "timer.triggered ignored: already firing");
        return;
    }
    evt_t e = { .type = EVT_FIRE, .reason = FIRE_REASON_TIMER };
    if (s_q) xQueueSend(s_q, &e, 0);
}

// ---------------------------------------------------------------------
// CLI helpers
// ---------------------------------------------------------------------

// Parse a boolean token. Accepts on/off, true/false, 1/0 (case-sensitive
// lowercase). Returns true on success and writes *out.
static bool parse_bool_token(const char *s, bool *out)
{
    if (!s || !out) return false;
    if (strcmp(s, "on")   == 0 || strcmp(s, "true")  == 0 || strcmp(s, "1") == 0) {
        *out = true; return true;
    }
    if (strcmp(s, "off")  == 0 || strcmp(s, "false") == 0 || strcmp(s, "0") == 0) {
        *out = false; return true;
    }
    return false;
}

// True if every char in `s` is a decimal digit (and `s` is non-empty).
static bool is_all_digits(const char *s)
{
    if (!s || !*s) return false;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// CLI handlers — per-property setters
// ---------------------------------------------------------------------

static sk_err_t cli_set_gpio(sk_cli_ctx_t *ctx)
{
    long v = -1;
    if (!sk_cli_arg_after_long(ctx, "gpio", &v)) {
        const char *s = sk_cli_arg(ctx, 0);
        if (s) v = atol(s);
    }
    const char *raw = sk_cli_arg(ctx, 0);
    bool present = sk_cli_arg_after(ctx, "gpio") != NULL || raw != NULL;
    if (!present) {
        sk_cli_usage(ctx,
            "relay gpio <N>",
            "  N: GPIO number, 0..30 on ESP32-C6",
            "Example: relay gpio 19");
        return SK_ERR_MISSING_ARG;
    }
    if (v < 0 || v > 30) {
        sk_cli_usage(ctx,
            "relay gpio <N>",
            "  N: GPIO number, 0..30",
            "Example: relay gpio 19");
        return SK_ERR_INVALID_ARG;
    }

    s_cfg.gpio_num = (int)v;
    nvs_save();

    gpio_reset_pin(s_cfg.gpio_num);
    gpio_set_direction(s_cfg.gpio_num, GPIO_MODE_OUTPUT);
    gpio_write_idle();

    if (!sk_cli_is_machine_mode(ctx)) {
        sk_cli_kvf(ctx, "GPIO", "%d", s_cfg.gpio_num);
        sk_cli_ok(ctx, NULL);
    } else {
        char data[48];
        snprintf(data, sizeof(data), "{\"gpio\":%d}", s_cfg.gpio_num);
        sk_cli_ok(ctx, data);
    }
    return SK_OK;
}

static sk_err_t cli_set_invert(sk_cli_ctx_t *ctx)
{
    const char *s = sk_cli_arg_after(ctx, "invert");
    if (!s) s = sk_cli_arg(ctx, 0);
    bool v;
    if (!s || !parse_bool_token(s, &v)) {
        sk_cli_usage(ctx,
            "relay invert <on|off>",
            "  on  : idle HIGH, active LOW\n"
            "  off : idle LOW, active HIGH (default)",
            "Example: relay invert on");
        return s ? SK_ERR_INVALID_ARG : SK_ERR_MISSING_ARG;
    }

    s_cfg.invert = v;
    nvs_save();
    // Re-assert idle level immediately so the wire reflects new polarity.
    if (s_cfg.gpio_num >= 0 && s_phase == LS_RELAY_IDLE) {
        gpio_write_idle();
    }

    if (!sk_cli_is_machine_mode(ctx)) {
        sk_cli_kv(ctx, "Invert", v ? "on" : "off");
        sk_cli_ok(ctx, NULL);
    } else {
        char data[32];
        snprintf(data, sizeof(data), "{\"invert\":%s}", v ? "true" : "false");
        sk_cli_ok(ctx, data);
    }
    return SK_OK;
}

static sk_err_t cli_set_delay(sk_cli_ctx_t *ctx)
{
    long v = -1;
    bool found = sk_cli_arg_after_long(ctx, "delay", &v);
    if (!found) {
        const char *s = sk_cli_arg(ctx, 0);
        if (s) { v = atol(s); found = true; }
    }
    if (!found) {
        sk_cli_usage(ctx,
            "relay delay <s>",
            "  s: start delay in seconds, 0..10",
            "Example: relay delay 5");
        return SK_ERR_MISSING_ARG;
    }
    if (v < 0 || v > (long)LS_RELAY_MAX_DELAY_SEC) {
        sk_cli_usage(ctx,
            "relay delay <s>",
            "  s: start delay in seconds, 0..10",
            "Example: relay delay 5");
        return SK_ERR_INVALID_ARG;
    }

    s_cfg.start_delay_sec = (uint16_t)v;
    nvs_save();

    if (!sk_cli_is_machine_mode(ctx)) {
        char dur[32];
        sk_cli_fmt_duration(dur, sizeof(dur), s_cfg.start_delay_sec);
        sk_cli_kvf(ctx, "Start delay", "%s", dur);
        sk_cli_ok(ctx, NULL);
    } else {
        char data[48];
        snprintf(data, sizeof(data), "{\"delay_sec\":%u}",
                 (unsigned)s_cfg.start_delay_sec);
        sk_cli_ok(ctx, data);
    }
    return SK_OK;
}

static sk_err_t cli_set_duration(sk_cli_ctx_t *ctx)
{
    long v = -1;
    bool found = sk_cli_arg_after_long(ctx, "duration", &v);
    if (!found) {
        const char *s = sk_cli_arg(ctx, 0);
        if (s) { v = atol(s); found = true; }
    }
    if (!found) {
        sk_cli_usage(ctx,
            "relay duration <s>",
            "  s: duration in seconds, 1..3600",
            "Example: relay duration 60");
        return SK_ERR_MISSING_ARG;
    }
    if (v < (long)LS_RELAY_MIN_DURATION_SEC || v > (long)LS_RELAY_MAX_DURATION_SEC) {
        sk_cli_usage(ctx,
            "relay duration <s>",
            "  s: duration in seconds, 1..3600",
            "Example: relay duration 60");
        return SK_ERR_INVALID_ARG;
    }

    s_cfg.total_duration_sec = (uint32_t)v;
    nvs_save();

    if (!sk_cli_is_machine_mode(ctx)) {
        char dur[32];
        sk_cli_fmt_duration(dur, sizeof(dur), s_cfg.total_duration_sec);
        sk_cli_kvf(ctx, "Active duration", "%s", dur);
        sk_cli_ok(ctx, NULL);
    } else {
        char data[48];
        snprintf(data, sizeof(data), "{\"duration_sec\":%u}",
                 (unsigned)s_cfg.total_duration_sec);
        sk_cli_ok(ctx, data);
    }
    return SK_OK;
}

static sk_err_t cli_set_pulse(sk_cli_ctx_t *ctx)
{
    const char *s = sk_cli_arg_after(ctx, "pulse");
    if (!s) s = sk_cli_arg(ctx, 0);
    if (!s) {
        sk_cli_usage(ctx,
            "relay pulse <on|off> | relay pulse <s>",
            "  on  : enable pulse mode (keep current half-cycle)\n"
            "  off : disable pulse mode (steady ON)\n"
            "  s   : enable pulse mode AND set half-cycle, 1..60 seconds",
            "Example: relay pulse 5");
        return SK_ERR_MISSING_ARG;
    }

    bool bool_val;
    if (parse_bool_token(s, &bool_val)) {
        // on/off toggle: leave pulse_duration_sec untouched.
        s_cfg.pulse_enabled = bool_val;
        nvs_save();

        if (!sk_cli_is_machine_mode(ctx)) {
            sk_cli_kv(ctx, "Pulse mode", bool_val ? "on" : "off");
            sk_cli_ok(ctx, NULL);
        } else {
            char data[64];
            snprintf(data, sizeof(data),
                "{\"pulse\":%s,\"pulse_duration_sec\":%u}",
                bool_val ? "true" : "false",
                (unsigned)s_cfg.pulse_duration_sec);
            sk_cli_ok(ctx, data);
        }
        return SK_OK;
    }

    if (!is_all_digits(s)) {
        sk_cli_usage(ctx,
            "relay pulse <on|off> | relay pulse <s>",
            "  on/off, or s (1..60 seconds)",
            "Example: relay pulse 5");
        return SK_ERR_INVALID_ARG;
    }

    long v = atol(s);
    if (v < (long)LS_RELAY_MIN_PULSE_SEC || v > (long)LS_RELAY_MAX_PULSE_SEC) {
        sk_cli_usage(ctx,
            "relay pulse <s>",
            "  s: half-cycle in seconds, 1..60",
            "Example: relay pulse 5");
        return SK_ERR_INVALID_ARG;
    }

    s_cfg.pulse_enabled = true;
    s_cfg.pulse_duration_sec = (uint16_t)v;
    nvs_save();

    if (!sk_cli_is_machine_mode(ctx)) {
        char dur[32];
        sk_cli_fmt_duration(dur, sizeof(dur), s_cfg.pulse_duration_sec);
        sk_cli_kv (ctx, "Pulse mode",     "on");
        sk_cli_kvf(ctx, "Pulse duration", "%s", dur);
        sk_cli_ok(ctx, NULL);
    } else {
        char data[64];
        snprintf(data, sizeof(data),
            "{\"pulse\":true,\"pulse_duration_sec\":%u}",
            (unsigned)s_cfg.pulse_duration_sec);
        sk_cli_ok(ctx, data);
    }
    return SK_OK;
}

static sk_err_t cli_get(sk_cli_ctx_t *ctx)
{
    if (!sk_cli_is_machine_mode(ctx)) {
        char dur[32];
        sk_cli_kvf(ctx, "GPIO",   "%d", s_cfg.gpio_num);
        sk_cli_kv (ctx, "Invert", s_cfg.invert ? "on" : "off");
        sk_cli_fmt_duration(dur, sizeof(dur), s_cfg.start_delay_sec);
        sk_cli_kvf(ctx, "Start delay",     "%s", dur);
        sk_cli_fmt_duration(dur, sizeof(dur), s_cfg.total_duration_sec);
        sk_cli_kvf(ctx, "Active duration", "%s", dur);
        sk_cli_kv (ctx, "Pulse mode", s_cfg.pulse_enabled ? "on" : "off");
        sk_cli_fmt_duration(dur, sizeof(dur), s_cfg.pulse_duration_sec);
        sk_cli_kvf(ctx, "Pulse duration", "%s", dur);
        sk_cli_ok(ctx, NULL);
        return SK_OK;
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"gpio\":%d,\"invert\":%s,\"delay_sec\":%u,"
        "\"duration_sec\":%u,\"pulse\":%s,\"pulse_duration_sec\":%u}",
        s_cfg.gpio_num,
        s_cfg.invert ? "true" : "false",
        (unsigned)s_cfg.start_delay_sec,
        (unsigned)s_cfg.total_duration_sec,
        s_cfg.pulse_enabled ? "true" : "false",
        (unsigned)s_cfg.pulse_duration_sec);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static sk_err_t cli_test(sk_cli_ctx_t *ctx)
{
    if (s_cfg.gpio_num < 0) {
        sk_cli_err(ctx, SK_ERR_INVALID_ARG,
            "{\"reason\":\"gpio not configured\"}");
        return SK_ERR_INVALID_ARG;
    }
    // Reject if already firing: otherwise the queue would buffer a FIRE
    // that fires immediately after the current sequence ends.
    if (s_phase != LS_RELAY_IDLE) {
        sk_cli_err(ctx, SK_ERR_BUSY,
            "{\"reason\":\"already firing; use relay.abort first\"}");
        return SK_ERR_BUSY;
    }
    evt_t e = { .type = EVT_FIRE, .reason = FIRE_REASON_MANUAL };
    BaseType_t ok = xQueueSend(s_q, &e, 0);
    if (ok != pdTRUE) {
        sk_cli_err(ctx, SK_ERR_BUSY,
            "{\"reason\":\"queue full\"}");
        return SK_ERR_BUSY;
    }

    if (!sk_cli_is_machine_mode(ctx)) {
        sk_cli_kv(ctx, "Result", "test started");
    }
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_off(sk_cli_ctx_t *ctx)
{
    // Force relay to idle. If a fire is in progress, request abort; the
    // worker's cancellable sleeps will pick it up and emit relay.fire.end.
    // Always drive the line to idle immediately as a safety net.
    bool was_firing = (s_phase != LS_RELAY_IDLE);
    if (was_firing) {
        atomic_store(&s_abort, true);
    }
    gpio_write_idle();

    if (!sk_cli_is_machine_mode(ctx)) {
        sk_cli_kv(ctx, "Result",
                  was_firing ? "sequence aborted, relay idle" : "relay idle");
    }
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_abort(sk_cli_ctx_t *ctx)
{
    if (s_phase == LS_RELAY_IDLE) {
        sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"not firing\"}");
        return SK_ERR_BUSY;
    }
    atomic_store(&s_abort, true);

    if (!sk_cli_is_machine_mode(ctx)) {
        sk_cli_kv(ctx, "Result", "abort requested");
    }
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_status(sk_cli_ctx_t *ctx)
{
    bool act  = atomic_load(&s_active);
    int level = act ? active_level() : idle_level();
    if (s_cfg.gpio_num < 0) level = -1;

    if (!sk_cli_is_machine_mode(ctx)) {
        sk_cli_kv (ctx, "Phase",  phase_str(s_phase));
        sk_cli_kvf(ctx, "GPIO",   "%d", s_cfg.gpio_num);
        sk_cli_kv (ctx, "Active", act ? "yes" : "no");
        if (s_fire_started_us > 0) {
            int64_t now = esp_timer_get_time();
            uint32_t since_sec = (uint32_t)((now - s_fire_started_us) / 1000000);
            char dur[32];
            sk_cli_fmt_duration(dur, sizeof(dur), since_sec);
            sk_cli_kvf(ctx, "Started", "%s ago", dur);
        }
        sk_cli_ok(ctx, NULL);
        return SK_OK;
    }

    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"phase\":\"%s\",\"gpio\":%d,\"gpio_level\":%d,"
        "\"active\":%s,\"fire_started_us\":%lld}",
        phase_str(s_phase),
        s_cfg.gpio_num, level,
        act ? "true" : "false",
        (long long)s_fire_started_us);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name = "relay.gpio",
      .summary = "Set relay output GPIO pin (0..30)",
      .usage   = "relay gpio <N>",
      .help_block =
          "Set the relay output GPIO pin.\n"
          "\n"
          "  N: GPIO number, 0..30 on ESP32-C6.\n"
          "     Default 19 (LS PCB rev-A D8 pin).\n"
          "\n"
          "Examples:\n"
          "  relay gpio 19\n"
          "  relay gpio 5\n"
          "\n"
          "Persists to NVS. Pin direction is reconfigured immediately;\n"
          "the line is driven to the idle level (or active if `relay invert on`\n"
          "was already set).",
      .handler = cli_set_gpio },
    { .name = "relay.invert",
      .summary = "Set relay polarity: on | off (idle/active level swap)",
      .usage   = "relay invert <on|off>",
      .help_block =
          "Invert the relay output polarity.\n"
          "\n"
          "  on  | true  | 1 : idle is HIGH (3.3V), active is LOW\n"
          "  off | false | 0 : idle is LOW (0V), active is HIGH  (default)\n"
          "\n"
          "Use this when the relay board is wired with an inverted optocoupler\n"
          "(common for SSR modules). The idle level is re-asserted immediately\n"
          "when toggled.\n"
          "\n"
          "Examples:\n"
          "  relay invert on\n"
          "  relay invert off",
      .handler = cli_set_invert },
    { .name = "relay.delay",
      .summary = "Set start delay in seconds (0..10)",
      .usage   = "relay delay <s>",
      .help_block =
          "Set the start delay before the relay fires.\n"
          "\n"
          "  s: seconds to wait after timer.triggered (or relay.test),\n"
          "     0..10. Default 0 (immediate).\n"
          "\n"
          "Useful to give peripherals (PC shutdown, alarm sound) a chance\n"
          "before the load is energized.\n"
          "\n"
          "Examples:\n"
          "  relay delay 0\n"
          "  relay delay 5",
      .handler = cli_set_delay },
    { .name = "relay.duration",
      .summary = "Set total active duration in seconds (1..3600)",
      .usage   = "relay duration <s>",
      .help_block =
          "Set the total active duration of the relay fire.\n"
          "\n"
          "  s: total seconds the relay should be active, 1..3600 (60 min cap).\n"
          "\n"
          "In pulse mode (see `relay pulse`), this is the TOTAL window during\n"
          "which the on/off cycles run.\n"
          "\n"
          "Examples:\n"
          "  relay duration 5      (fire for 5 seconds)\n"
          "  relay duration 60     (fire for 1 minute, default)",
      .handler = cli_set_duration },
    { .name = "relay.pulse",
      .summary = "Pulse mode: on | off | <half-cycle seconds 1..60>",
      .usage   = "relay pulse <on|off> | relay pulse <s>",
      .help_block =
          "Configure pulse mode (on/off cycling during fire).\n"
          "\n"
          "  <s>      Enable pulse mode and set the half-cycle duration to\n"
          "           s seconds (1..60). The relay alternates s seconds ON,\n"
          "           s seconds OFF for the entire `relay.duration` window.\n"
          "\n"
          "  on       Enable pulse mode keeping the current half-cycle.\n"
          "  off      Disable pulse mode (steady ON for `relay.duration`).\n"
          "\n"
          "Examples:\n"
          "  relay pulse 5         (5s on / 5s off cycles)\n"
          "  relay pulse off       (steady-on mode)\n"
          "  relay pulse on        (re-enable previous half-cycle)\n"
          "\n"
          "Number of cycles = relay.duration / (2 * pulse half-cycle).",
      .handler = cli_set_pulse },
    { .name = "relay.get",
      .summary = "Show current relay configuration",
      .usage   = "relay get",
      .help_block =
          "Show the current relay configuration.\n"
          "\n"
          "No arguments.",
      .handler = cli_get },
    { .name = "relay.test",
      .summary = "Manually fire the relay using current config",
      .usage   = "relay test",
      .help_block =
          "Manually run a full fire sequence using the current config.\n"
          "\n"
          "No arguments. Same behavior as if timer.triggered fired now:\n"
          "delay -> active (or pulse cycles) -> idle. Use `relay abort` to\n"
          "cancel mid-fire.\n"
          "\n"
          "Examples:\n"
          "  relay test",
      .handler = cli_test },
    { .name = "relay.fire",
      .summary = "Alias of relay.test",
      .usage   = "relay fire",
      .help_block = "Alias of relay.test.",
      .hidden  = true,
      .handler = cli_test },
    { .name = "relay.abort",
      .summary = "Abort an in-progress fire sequence",
      .usage   = "relay abort",
      .help_block =
          "Cancel an in-progress fire sequence.\n"
          "\n"
          "No arguments. Returns ERR_BUSY if the relay is already idle.\n"
          "The GPIO is forced to idle and a `relay.fire.end` event is\n"
          "emitted with aborted=true.",
      .handler = cli_abort },
    { .name = "relay.off",
      .summary = "Force relay to idle (also aborts in-progress fire)",
      .usage   = "relay off",
      .help_block =
          "Force the relay to its idle level immediately (kill switch).\n"
          "\n"
          "No arguments. Aborts any in-progress fire AND drives the GPIO\n"
          "to idle, ensuring the wire is de-energized regardless of state.\n"
          "Safe to call at any time.\n"
          "\n"
          "Examples:\n"
          "  relay off",
      .handler = cli_off },
    { .name = "relay.status",
      .summary = "Show current relay runtime status",
      .usage   = "relay status",
      .help_block =
          "Show the relay's current runtime state.\n"
          "\n"
          "Reports the phase (idle / delaying / active / pulsing), the GPIO\n"
          "pin, and whether the line is currently driven active. Includes\n"
          "the time since fire start when not idle.",
      .handler = cli_status },
};

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

void ls_relay_status(ls_relay_status_t *out)
{
    if (!out) return;
    bool act = atomic_load(&s_active);
    int level = act ? active_level() : idle_level();
    out->phase             = s_phase;
    out->gpio_level        = (level == 1);
    out->gpio_active       = act;
    out->fire_started_us   = s_fire_started_us;
}

void ls_relay_config(ls_relay_config_t *out)
{
    if (!out) return;
    *out = s_cfg;
}

esp_err_t ls_relay_init(int default_gpio_num)
{
    s_default_gpio = default_gpio_num;
    nvs_load(default_gpio_num);

    if (s_cfg.gpio_num >= 0) {
        gpio_reset_pin(s_cfg.gpio_num);
        esp_err_t err = gpio_set_direction(s_cfg.gpio_num, GPIO_MODE_OUTPUT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_set_direction(%d) failed: %s",
                     s_cfg.gpio_num, esp_err_to_name(err));
        }
        gpio_write_idle();
    } else {
        ESP_LOGW(TAG, "gpio_num < 0, relay disabled at boot");
    }

    s_q = xQueueCreate(2, sizeof(evt_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(worker_task, "ls_relay", 3072, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    // Subscribe to timer.triggered for auto-fire.
    int sub;
    sk_event_bus_subscribe("timer.triggered", on_timer_triggered, NULL, &sub);

    // Factory reset hook — drive GPIO idle, abort fire, wipe NVS.
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, &sub);

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); ++i) {
        sk_cli_register(&s_cmds[i]);
    }

    ESP_LOGI(TAG, "init: gpio=%d invert=%d delay=%us dur=%us pulse=%d/%us",
             s_cfg.gpio_num, (int)s_cfg.invert,
             (unsigned)s_cfg.start_delay_sec,
             (unsigned)s_cfg.total_duration_sec,
             (int)s_cfg.pulse_enabled,
             (unsigned)s_cfg.pulse_duration_sec);

    return ESP_OK;
}
