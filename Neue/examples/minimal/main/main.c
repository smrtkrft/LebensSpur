// Minimal SKAPP-compatible device.
//
// Boots sk_core, starts the USB Serial/JTAG CLI, and registers one trivial
// device-specific command ("demo.ping") to show the pattern. Use this as a
// starting point for a new SmartKraft device by copying the folder and
// adding your own components and CLI commands.

#include <stdio.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "sk_core.h"
#include "sk_transport_usb.h"

static const char *TAG = "main";

// Example device-specific command.
static sk_err_t cmd_demo_ping(sk_cli_ctx_t *ctx)
{
    sk_cli_ok(ctx, "{\"pong\":true}");
    return SK_OK;
}

static const sk_cli_command_t cmd_ping = {
    .name       = "demo.ping",
    .summary    = "Reply with pong",
    .usage      = "demo ping",
    .handler    = cmd_demo_ping,
};

void app_main(void)
{
    // Standard NVS bootstrap.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(sk_core_init(&(sk_core_cfg_t){
        .device_type_prefix = "MN",       // 2-char type code
        .hw_rev             = 'A',        // hardware revision letter
        .fw_version         = "0.1.0",
        .build_info         = NULL,
    }));

    ESP_ERROR_CHECK(sk_cli_register(&cmd_ping));

    ESP_ERROR_CHECK(sk_transport_usb_init(NULL));

    ESP_LOGI(TAG, "minimal device up — id=%s", sk_identity_get());
}
