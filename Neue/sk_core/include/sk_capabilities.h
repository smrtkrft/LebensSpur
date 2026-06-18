#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;       // e.g. "sk_core", "sk_transport_ble"
    const char *version;    // semver "1.2.0"
} sk_capability_book_t;

// Register a book as loaded. Each sk_* library calls this during its init so
// the capability set reflects reality (not just compile-time linkage).
esp_err_t sk_capabilities_register_book(const char *name, const char *version);

// Capability initialization — pull all already-registered books plus register
// the built-in `device.capabilities` CLI command. Call after sk_cli_init().
esp_err_t sk_capabilities_init(const char *fw_version);

// Emit the full capability manifest as a JSON object to `out`. Returns
// bytes written (excluding null). Truncates if `out_cap` is too small.
size_t sk_capabilities_render_json(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
