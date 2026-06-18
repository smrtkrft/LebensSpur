#include "sk_capabilities.h"
#include "sk_cli.h"
#include "sk_identity.h"
#include "sk_errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "sk_capabilities";

#define SK_CAP_MAX_BOOKS 24

typedef struct {
    char name[32];
    char version[16];
    bool in_use;
} book_slot_t;

static book_slot_t s_books[SK_CAP_MAX_BOOKS];
static char        s_fw_version[16] = "";
static bool        s_ready           = false;

esp_err_t sk_capabilities_register_book(const char *name, const char *version)
{
    if (!name || !version) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < SK_CAP_MAX_BOOKS; i++) {
        if (s_books[i].in_use && strcmp(s_books[i].name, name) == 0) {
            strncpy(s_books[i].version, version, sizeof(s_books[i].version) - 1);
            s_books[i].version[sizeof(s_books[i].version) - 1] = '\0';
            return ESP_OK;
        }
    }
    for (int i = 0; i < SK_CAP_MAX_BOOKS; i++) {
        if (!s_books[i].in_use) {
            strncpy(s_books[i].name, name, sizeof(s_books[i].name) - 1);
            s_books[i].name[sizeof(s_books[i].name) - 1] = '\0';
            strncpy(s_books[i].version, version, sizeof(s_books[i].version) - 1);
            s_books[i].version[sizeof(s_books[i].version) - 1] = '\0';
            s_books[i].in_use = true;
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "book table full, dropping %s", name);
    return ESP_ERR_NO_MEM;
}

size_t sk_capabilities_render_json(char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return 0;
    size_t off = 0;
    int n = snprintf(out + off, out_cap - off,
                     "{\"device_id\":\"%s\",\"fw_version\":\"%s\",\"books\":[",
                     sk_identity_get(), s_fw_version);
    if (n < 0 || (size_t)n >= out_cap - off) return out_cap - 1;
    off += (size_t)n;

    bool first = true;
    for (int i = 0; i < SK_CAP_MAX_BOOKS; i++) {
        if (!s_books[i].in_use) continue;
        n = snprintf(out + off, out_cap - off,
                     "%s{\"name\":\"%s\",\"version\":\"%s\"}",
                     first ? "" : ",", s_books[i].name, s_books[i].version);
        if (n < 0 || (size_t)n >= out_cap - off) return out_cap - 1;
        off += (size_t)n;
        first = false;
    }
    n = snprintf(out + off, out_cap - off, "]}");
    if (n < 0 || (size_t)n >= out_cap - off) return out_cap - 1;
    off += (size_t)n;
    return off;
}

static sk_err_t cmd_device_capabilities(sk_cli_ctx_t *ctx)
{
    enum { BUF = 1024 };
    // Heap-allocated to keep the CLI task stack bounded as the capability
    // list grows (each registered book/handler adds a fragment).
    char *buf = malloc(BUF);
    if (!buf) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"oom\"}");
        return SK_OK;
    }
    sk_capabilities_render_json(buf, BUF);
    sk_cli_ok(ctx, buf);
    free(buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmd_capabilities = {
    .name       = "device.capabilities",
    .summary    = "List loaded books and command versions",
    .usage      = "device capabilities",
    .help_block = "Reports which sk_* books are active on this device and the\n"
                  "firmware version. APP uses this to gate UI features.",
    .hidden     = true,    // SKAPP-only fingerprint; humans never type this.
    .handler    = cmd_device_capabilities,
};

esp_err_t sk_capabilities_init(const char *fw_version)
{
    if (s_ready) return ESP_OK;
    if (fw_version) {
        strncpy(s_fw_version, fw_version, sizeof(s_fw_version) - 1);
        s_fw_version[sizeof(s_fw_version) - 1] = '\0';
    }
    sk_capabilities_register_book("sk_core", "0.1.0");
    sk_cli_register(&s_cmd_capabilities);
    s_ready = true;
    return ESP_OK;
}
