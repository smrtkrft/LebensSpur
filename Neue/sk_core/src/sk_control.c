// Device-wide control button gesture interpreter and the matching
// `device.restart` / `device.factory-reset` CLI commands.
//
// Subscribes to "button.released" (sk_button publishes on every release with
// duration_ms) and dispatches:
//   < 3 s          → publish "control.short-press"
//                    (sk_auth opens 60 s BLE window — pairing if unbonded,
//                    reachable if bonded; same gesture for both)
//   3-5 s          → ignored (debounce zone, prevents accidental restart)
//   5-10 s         → device restart
//   >= 10 s        → device factory reset (publishes
//                    "device.factory-reset.requested" so subscribers like
//                    sk_auth and sk_api (plus any device-specific
//                    cleanup) can run, brief delay, then esp_restart)
//
// Also subscribes to "button.multi-tap" — sk_button fires that event
// when the user taps the control button N times within multi_tap_window_ms
// (default 5 taps / 800 ms). This is a second, independently-discoverable
// path to factory reset for users who can't physically hold the button
// down for 10 s (sticky button, glove, etc.).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sk_auth.h"
#include "sk_capabilities.h"
#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"

static const char *TAG = "sk_control";

// Gesture thresholds (ms). 3-5 s is a deliberate dead-zone so a clumsy
// short press doesn't accidentally restart the device, but a real
// "hold for ~5 seconds" reliably triggers a reboot. Holds past 10 s
// escalate to factory-reset — the physical hold IS the confirm, so no
// CLI confirm token is required for either path.
//   < 3 s          → control.short-press (BLE pair window)
//   3-5 s          → ignored (debounce zone)
//   5-10 s         → device restart
//   ≥ 10 s         → device factory reset
#define SHORT_PRESS_MAX_MS    3000
#define RESTART_MIN_MS        5000
#define FACTORY_RESET_MIN_MS  10000

// -- Gesture interpreter ----------------------------------------------------

static void on_button_released(const sk_event_t *evt, void *user)
{
    (void)user;
    if (!evt || !evt->payload_json) return;

    const char *p = strstr(evt->payload_json, "\"duration_ms\"");
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p++;
    long ms = strtol(p, NULL, 10);
    if (ms <= 0) return;

    // Diagnostic: log every release with measured duration. Lets us tell
    // a "the user actually held for 5 s" from a "GPIO bounced and the
    // duration counter went haywire" without guessing.
    ESP_LOGI(TAG, "button.released measured duration = %ld ms", ms);

    if (ms < SHORT_PRESS_MAX_MS) {
        ESP_LOGI(TAG, "short press → control.short-press");
        sk_event_bus_publish("control.short-press", NULL);
    } else if (ms >= FACTORY_RESET_MIN_MS) {
        ESP_LOGW(TAG, "very long press (%ld ms) → device.factory-reset", ms);
        // Subscribers (sk_auth wipes bond, sk_api wipes endpoints, any
        // device-specific component subscribed for cleanup) get a brief
        // window to commit NVS erases before we esp_restart.
        sk_event_bus_publish("device.factory-reset.requested",
                             "{\"reason\":\"button_10s_hold\"}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (ms >= RESTART_MIN_MS) {
        ESP_LOGW(TAG, "long press (%ld ms) → device.restart", ms);
        sk_event_bus_publish("device.shutdown.imminent",
                             "{\"reason\":\"button_long_press\","
                             "\"delay_ms\":300}");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else {
        // 3-5 s dead-zone.
        ESP_LOGI(TAG, "long press in dead-zone (%ld ms) — ignored", ms);
    }
}

// Second physical path to factory reset: 5× rapid tap on the control
// button. sk_button fires `button.multi-tap` once the threshold is met;
// we route it through the same `device.factory-reset.requested` event
// so subscribers don't need to know which gesture triggered the wipe.
static void on_button_multi_tap(const sk_event_t *evt, void *user)
{
    (void)user;
    int count = -1;
    if (evt && evt->payload_json) {
        const char *p = strstr(evt->payload_json, "\"count\"");
        if (p && (p = strchr(p, ':')) != NULL) {
            count = (int)strtol(p + 1, NULL, 10);
        }
    }
    ESP_LOGW(TAG, "multi-tap (×%d) → device.factory-reset", count);
    sk_event_bus_publish("device.factory-reset.requested",
                         "{\"reason\":\"multi_tap\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// -- CLI commands -----------------------------------------------------------

static sk_err_t cmd_device_restart(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    sk_cli_ok(ctx, "{\"restarting\":true}");
    sk_event_bus_publish("device.shutdown.imminent",
                         "{\"reason\":\"cli_restart\",\"delay_ms\":500}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return SK_OK;
}

static sk_err_t cmd_device_factory_reset(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    sk_cli_ok(ctx, "{\"factory_reset\":true}");
    // Payload mirrors the button-driven paths (button_10s_hold,
    // multi_tap) so subscribers can log a single, uniform reason field.
    sk_event_bus_publish("device.factory-reset.requested",
                         "{\"reason\":\"cli\"}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name = "device.restart",
      .summary = "Restart the device",
      .usage   = "device restart",
      .critical = true,
      .help_block =
          "Soft reboot. Active timer is lost; user settings (NVS) are kept.\n"
          "Critical — first call returns a ready-to-paste retry command\n"
          "with a fresh confirm token. See `help device.confirm-token`.",
      .handler = cmd_device_restart },

    { .name = "device.factory-reset",
      .summary = "Wipe bond, settings, return to factory state",
      .usage   = "device factory-reset",
      .critical = true,
      .help_block =
          "Erases ALL stored data: WiFi credentials, API endpoints, BLE\n"
          "bond, secure store, userdata blob, every NVS namespace. Reboots\n"
          "into a factory-fresh state afterwards.\n"
          "\n"
          "Equivalent to the 5-tap MULTI_TAP button gesture.\n"
          "\n"
          "Critical — first call returns a ready-to-paste retry command\n"
          "with a fresh confirm token. See `help device.confirm-token`.",
      .handler = cmd_device_factory_reset },
};

// -- Init -------------------------------------------------------------------

esp_err_t sk_control_init(void)
{
    int sub;
    esp_err_t err = sk_event_bus_subscribe("button.released",
                                           on_button_released, NULL, &sub);
    if (err != ESP_OK) return err;
    sk_event_bus_subscribe("button.multi-tap", on_button_multi_tap, NULL, &sub);

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        sk_cli_register(&s_cmds[i]);
    }
    sk_capabilities_register_book("sk_control", "0.1.0");
    ESP_LOGI(TAG, "control button gesture interpreter ready");
    return ESP_OK;
}
