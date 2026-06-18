#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *prompt;        // override prompt; NULL = use sk_core_get_prompt() ("BF-A06TMFSQT> ")
    int         history_size;  // RESERVED (was linenoise history; new direct-driver path stores no history)
    int         task_stack;    // FreeRTOS task stack bytes, default 8192
    int         task_priority; // default 4
} sk_transport_usb_cfg_t;

// Install USB Serial/JTAG driver (direct path, no linenoise / no stdio /
// no VFS) and spawn the CLI task that reads lines from
// `usb_serial_jtag_read_bytes` and dispatches them through sk_cli. Echo +
// prompt only when the line starts with anything other than `{` (human
// mode); JSON / machine clients (SKAPP USB Console) see clean response
// stream with no banner noise.
//
// Lives inside sk_core as of 2026-04-26 — every SmartKraft device gets the
// USB CLI bundled by default for development/onboarding. Pass `cfg = NULL`
// to accept all defaults. Safe to call once; subsequent calls are no-ops.
esp_err_t sk_transport_usb_init(const sk_transport_usb_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
