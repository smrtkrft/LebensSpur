#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Device ID format: "XX-YZZZZZZZZ"
//   XX        : 2-char device type prefix (e.g. "LS" for LebensSpur)
//   Y         : 1-char hardware revision letter (A = first batch, B = second...)
//   ZZZZZZZZ  : 8-char unique tail derived deterministically from the factory MAC
//
// The tail is SHA-256(MAC) truncated to 40 bits and rendered in a 32-symbol
// Crockford alphabet (0-9, A-Z minus I, L, O, U). Globally unique by IEEE OUI
// allocation, stable across reboots, and produced without NVS, RTC, or any
// external time/random source.
#define SK_IDENTITY_PREFIX_LEN  2
#define SK_IDENTITY_HW_REV_LEN  1
#define SK_IDENTITY_UNIQUE_LEN  8
#define SK_IDENTITY_SUFFIX_LEN  (SK_IDENTITY_HW_REV_LEN + SK_IDENTITY_UNIQUE_LEN)
#define SK_IDENTITY_FULL_LEN    (SK_IDENTITY_PREFIX_LEN + 1 + SK_IDENTITY_SUFFIX_LEN)  // 12

// Initialize identity. Must be called once during boot, before BLE/mDNS
// advertising or any other sk_identity_* call.
//
// `prefix` must be exactly 2 uppercase ASCII characters (A-Z) identifying the
// device type — e.g. "LS" for LebensSpur. `hw_rev` is a single uppercase
// letter (A-Z) identifying the hardware revision; bump for each fab batch.
esp_err_t sk_identity_init(const char *prefix, char hw_rev);

// Returns the full device ID, e.g. "LS-AK7M2X9P". Stable across boots on
// the same hardware. Never NULL after successful init.
const char *sk_identity_get(void);

// Returns just the prefix, e.g. "LS".
const char *sk_identity_get_prefix(void);

// Returns the 9-char suffix (hw_rev + unique tail), e.g. "AK7M2X9P".
const char *sk_identity_get_suffix(void);

// Returns just the hardware revision letter.
char sk_identity_get_hw_rev(void);

// Returns the 8-char unique tail.
const char *sk_identity_get_unique(void);

// Copies the raw 6-byte factory MAC into `out`. Useful for BLE address
// derivation and telemetry.
esp_err_t sk_identity_get_mac(uint8_t out[6]);

#ifdef __cplusplus
}
#endif
