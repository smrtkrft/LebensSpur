#pragma once

// sk_passphrase — optional content-access passphrase layer.
//
// On top of the ECDH bond + Mutual C-R, the user may set a personal
// passphrase that the device demands before a session can issue commands.
// Two independent toggles control *when* the device asks for it:
//
//   pairing_required  — ask during the very first ECDH pairing of a new
//                       SKAPP install. The bond is only persisted after a
//                       correct passphrase. Stops "stolen device + button
//                       short-press" attacks.
//
//   always_required   — ask on every authenticated session opening, both
//                       pairing and reconnect. Stops "stolen, already-
//                       paired SKAPP" attacks.
//
// Storage: salt + PBKDF2-SHA256(100k) hash + mode bitmask + fail counter
// live in their own NVS namespace (`sk_pass`). The hash never leaves the
// device. fail_count persists across reboots so an attacker cannot reset
// it by power-cycling. Reaching the lockout limit triggers a factory
// reset.
//
// USB transport bypasses every check — physical access to the USB port
// already implies factory-reset capability via the control button.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SK_PASSPHRASE_MIN_LEN          6
#define SK_PASSPHRASE_MAX_LEN          32
#define SK_PASSPHRASE_SALT_LEN         16
#define SK_PASSPHRASE_HASH_LEN         32
// On ESP32-C6 each PBKDF2 iter pays a SHA peripheral acquire/release
// (mbedtls IDF port releases hw on hmac_finish and re-acquires on the
// next starts), which dominates raw block timing — measured ~2 ms/iter.
// 5000 -> ~10 s verify (observed in field, IDLE-starved WDT). 600 keeps
// verify under ~1.5 s worst case while still imposing some cost on an
// offline attacker with a flash dump. A short alphanumeric passphrase
// falls quickly to GPU brute force regardless of iteration count, so the
// real security guarantee is the 10-attempt lockout (factory-resets the
// device) — not KDF stretching.
#define SK_PASSPHRASE_PBKDF2_ITERS     600
#define SK_PASSPHRASE_FAIL_LOCKOUT     10    // factory-reset on Nth wrong attempt

typedef struct {
    bool pairing_required;   // ask during first-time pairing
    bool always_required;    // ask on every session opening
} sk_pass_mode_t;

// Initialize: load salt/hash/mode/fail_count from NVS, register CLI handlers,
// subscribe to factory-reset event. Idempotent.
esp_err_t sk_passphrase_init(void);

// True if the user has configured a passphrase.
bool sk_passphrase_is_set(void);

// Read the two mode toggles. If no passphrase is set, both fields are false.
sk_pass_mode_t sk_passphrase_get_mode(void);

// Update the toggles. Caller must already have authenticated; we don't gate
// here, dispatcher does. Persisted to NVS.
esp_err_t sk_passphrase_set_mode(bool pairing_required, bool always_required);

// Set a passphrase for the first time. Fails ESP_ERR_INVALID_STATE if one
// already exists — caller must use sk_passphrase_change instead. The salt
// is randomised; the hash is PBKDF2-SHA256(100k). Resets fail_count.
esp_err_t sk_passphrase_set(const char *plain);

// Change the passphrase. Verifies `old_plain` against the stored hash before
// writing. Wrong old → ESP_ERR_INVALID_RESPONSE; passes through fail_count
// bookkeeping (so a brute-force change attempt also triggers lockout).
esp_err_t sk_passphrase_change(const char *old_plain, const char *new_plain);

// Clear the passphrase. Verifies `old_plain` first. After clear, both mode
// toggles are forced off and fail_count is reset.
esp_err_t sk_passphrase_clear(const char *old_plain);

// Verify a candidate passphrase. ESP_OK → matches; counter is reset.
// ESP_ERR_INVALID_RESPONSE → mismatch; counter incremented and persisted.
// ESP_ERR_INVALID_STATE → no passphrase configured. Reaching the lockout
// limit publishes `device.factory-reset.requested` (the standard hook
// other modules already subscribe to) before returning.
//
// On return, *out_attempts_left (if non-NULL) is filled with how many wrong
// attempts remain before lockout (matches=lockout-N, mismatches=lockout-(N+1)
// after the increment). Caller surfaces this number to the SKAPP user.
esp_err_t sk_passphrase_verify(const char *plain, uint8_t *out_attempts_left);

// How many failed attempts have accumulated. 0 after a successful verify or
// a fresh `set`. Persists across reboots (NVS-backed).
uint8_t sk_passphrase_fail_count(void);

// Convenience for callers that want "how many tries left right now" without
// performing a verify.
uint8_t sk_passphrase_attempts_left(void);

#ifdef __cplusplus
}
#endif
