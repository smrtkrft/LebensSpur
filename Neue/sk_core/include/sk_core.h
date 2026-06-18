#pragma once

// Umbrella header — pulls every public sk_core API. Including sk_core.h
// alone is enough for application code; individual sub-headers can still
// be included directly when stricter dependencies are wanted.

#include <stddef.h>

#include "sk_identity.h"
#include "sk_errors.h"
#include "sk_event_bus.h"
#include "sk_cli.h"
#include "sk_capabilities.h"
#include "sk_baseline.h"

// Transports
#include "sk_transport_usb.h"
#include "sk_transport_ble.h"
#include "sk_transport_tcp.h"

// Network
#include "sk_wifi.h"
#include "sk_mdns.h"

// Secure session primitives (auth = pairing + handshake + HMAC + confirm)
#include "sk_auth.h"
#include "sk_passphrase.h"

// I/O hardware abstraction
#include "sk_button.h"
#include "sk_led.h"

// Firmware OTA
#include "sk_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_type_prefix;   // 2 char, e.g. "LS"
    char        hw_rev;               // single uppercase letter, 'A' = first hw revision
    const char *fw_version;           // semver string, e.g. "1.0.0"
    const char *build_info;           // optional git sha / build tag, can be NULL
} sk_core_cfg_t;

// Boot sequence wrapper. Calls identity_init, event_bus_init, errors_init,
// cli_init, capabilities_init in the right order. Must be the first sk_*
// function called in app_main (after nvs_flash_init).
esp_err_t sk_core_init(const sk_core_cfg_t *cfg);

// Returns the CLI prompt string. Built lazily from the device identity:
// "BF-A06TMFSQT> ". Falls back to "sk> " before sk_identity_init runs.
// Multi-device disambiguation when several units are plugged in is now
// trivial — the prompt itself shows which device you're talking to.
const char *sk_core_get_prompt(void);

// Per-project product name + version. The brand "SmartKraft" is fixed in
// sk_core. Each device project should call this once at boot, after
// sk_core_init, before any banner is printed:
//   sk_core_set_product("BlockingFocus", "1.0");
// `version` may be NULL — in that case sk_core falls back to the
// fw_version passed in sk_core_cfg_t. Both pointers must have static
// lifetime; sk_core stores them without copying.
void sk_core_set_product(const char *name, const char *version);

// Status-line provider. Fills the parenthesized portion of the connect
// banner:
//   BF-A06TMFSQT - SmartKraft BlockingFocus v1.0 (idle, battery 87%, wifi: connected)
//                                                  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
// Return the number of bytes written to `out` (excluding NUL). Return 0
// to omit the parens. If no provider is registered the parens are also
// omitted — devices without a meaningful runtime status (no battery, no
// wifi, ...) just don't register one.
typedef size_t (*sk_status_provider_t)(char *out, size_t cap);
void sk_core_set_status_provider(sk_status_provider_t fn);

// Render the connect-time / help-header banner via the supplied writer.
// Two lines:
//   <identity> - SmartKraft <product> v<version> (<status>)
//   Type 'help' for topics, TAB to autocomplete.
// Used by every human-mode transport on connect, and by the `help`
// command at the top of its output.
void sk_core_write_banner(sk_cli_writer_t writer, void *user);

#ifdef __cplusplus
}
#endif
