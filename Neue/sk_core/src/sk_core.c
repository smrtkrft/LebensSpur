#include "sk_core.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "sk_core";

#define SK_BRAND  "SmartKraft"

static const char            *s_fw_version       = "";
static const char            *s_product_name     = "";
static const char            *s_product_version  = "";   // empty = fall back to fw_version
static sk_status_provider_t   s_status_provider  = NULL;

// Prompt is built lazily on first call so identity has time to init.
// Static buffer — sk_transport_usb caches the pointer between read_line
// + write calls, so it must outlive every prompt invocation.
static char s_prompt_buf[SK_IDENTITY_FULL_LEN + 4];  // "BF-A06TMFSQT> " + NUL
static bool s_prompt_built = false;

const char *sk_core_get_prompt(void)
{
    if (s_prompt_built) return s_prompt_buf;

    const char *id = sk_identity_get();
    if (id && id[0]) {
        snprintf(s_prompt_buf, sizeof(s_prompt_buf), "%s> ", id);
        s_prompt_built = true;
        return s_prompt_buf;
    }
    return "sk> ";  // identity not ready yet — generic fallback
}

void sk_core_set_product(const char *name, const char *version)
{
    s_product_name    = name    ? name    : "";
    s_product_version = version ? version : "";
}

void sk_core_set_status_provider(sk_status_provider_t fn)
{
    s_status_provider = fn;
}

static const char *active_product_version(void)
{
    return s_product_version[0] ? s_product_version : s_fw_version;
}

void sk_core_write_banner(sk_cli_writer_t writer, void *user)
{
    if (!writer) return;

    char        line[256];
    int         n   = 0;
    const char *id  = sk_identity_get();
    const char *ver = active_product_version();

    if (s_product_name[0]) {
        if (ver[0]) {
            n = snprintf(line, sizeof(line), "%s - %s %s v%s",
                         id, SK_BRAND, s_product_name, ver);
        } else {
            n = snprintf(line, sizeof(line), "%s - %s %s",
                         id, SK_BRAND, s_product_name);
        }
    } else {
        n = snprintf(line, sizeof(line), "%s - %s", id, SK_BRAND);
    }
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

    // Optional status section in parens. Omitted when no provider, or the
    // provider returns 0 (e.g. battery-less device with nothing to report).
    if (s_status_provider) {
        char status[160];
        size_t slen = s_status_provider(status, sizeof(status));
        if (slen > 0 && slen < sizeof(status)) {
            int extra = snprintf(line + n, sizeof(line) - (size_t)n,
                                 " (%s)", status);
            if (extra > 0) {
                n += extra;
                if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
            }
        }
    }

    writer(line, (size_t)n, user);
    writer("\n", 1, user);
    static const char tip[] = "Type 'help' for topics, TAB to autocomplete.\n";
    writer(tip, sizeof(tip) - 1, user);
}

// Defined in sk_control.c — wires button.released into the gesture
// interpreter and registers device.restart/factory-reset CLI commands.
extern esp_err_t sk_control_init(void);

esp_err_t sk_core_init(const sk_core_cfg_t *cfg)
{
    if (!cfg || !cfg->device_type_prefix || !cfg->fw_version) {
        return ESP_ERR_INVALID_ARG;
    }
    s_fw_version = cfg->fw_version;

    esp_err_t err;
    if ((err = sk_identity_init(cfg->device_type_prefix, cfg->hw_rev)) != ESP_OK) return err;
    if ((err = sk_event_bus_init())                        != ESP_OK) return err;
    if ((err = sk_cli_init())                              != ESP_OK) return err;
    if ((err = sk_capabilities_init(cfg->fw_version))      != ESP_OK) return err;
    if ((err = sk_baseline_init(cfg->fw_version, cfg->build_info)) != ESP_OK) return err;
    if ((err = sk_control_init())                          != ESP_OK) return err;

    ESP_LOGI(TAG, "sk_core ready: device=%s fw=%s",
             sk_identity_get(), cfg->fw_version);
    return ESP_OK;
}
