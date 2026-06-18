#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SK_AUTH_TOKEN_LEN          32
#define SK_AUTH_CHALLENGE_LEN      16
#define SK_AUTH_RESPONSE_LEN       16
#define SK_AUTH_HMAC_LEN           16   // truncated HMAC-SHA256 prefix
#define SK_AUTH_ECDH_PUBLIC_LEN    32   // X25519
#define SK_AUTH_CONFIRM_TOKEN_LEN  32   // hex string chars (no null)
#define SK_AUTH_CONFIRM_TTL_SEC    30

#define SK_AUTH_PEER_ID_LEN        16   // SKAPP install UUID, 16 raw bytes
#define SK_AUTH_BOND_LABEL_MAX     24   // user-visible label, null-terminated
#define SK_AUTH_BOND_SLOT_COUNT    8    // multi-bond capacity
#define SK_AUTH_BOND_SLOT_INVALID  0xFF

typedef enum {
    SK_AUTH_PAIRING_IDLE,
    SK_AUTH_PAIRING_OPEN,
} sk_auth_pairing_state_t;

// -- Lifecycle -------------------------------------------------------------

// Initialize auth state: loads bond/token from NVS, wires confirm-token and
// pairing CLI commands.
esp_err_t sk_auth_init(void);

// True if at least one bond slot is occupied (device is paired).
bool sk_auth_has_bond(void);

// Clear all bond slots, active session, and confirm state. Called by factory
// reset.
esp_err_t sk_auth_clear_all(void);

// Replace the active bond's key with a freshly generated one. Disconnects
// active sessions (the caller typically follows up with a transport drop).
// If no active session is set, falls back to slot 0.
esp_err_t sk_auth_rotate_token(void);

// -- Multi-bond store ------------------------------------------------------
//
// The device keeps up to SK_AUTH_BOND_SLOT_COUNT bonds. Each bond ties a
// SKAPP install (peer_id) to a 32-byte session key. Slot index is stable
// across reboots; SKAPP receives the slot it was assigned during pairing
// so it can ask the device to remove/inspect that specific slot later.
typedef struct {
    uint8_t  slot;                                   // 0..SK_AUTH_BOND_SLOT_COUNT-1
    uint8_t  peer_id[SK_AUTH_PEER_ID_LEN];
    char     label[SK_AUTH_BOND_LABEL_MAX + 1];      // null-terminated UTF-8
    int64_t  paired_at_unix;                         // 0 if device had no SNTP
} sk_auth_bond_info_t;

// Add a bond. If an existing slot already holds the same peer_id, that slot
// is overwritten (re-pair from the same SKAPP install). Otherwise the lowest
// free slot is used. Returns ESP_ERR_NO_MEM if every slot is occupied — caller
// must remove a slot first (BOND_STORE_FULL UX). out_slot is filled on
// success.
esp_err_t sk_auth_bond_add(const uint8_t peer_id[SK_AUTH_PEER_ID_LEN],
                           const uint8_t bond_key[SK_AUTH_TOKEN_LEN],
                           const char    *label,
                           uint8_t       *out_slot);

// Remove bond by slot index. Active session is invalidated if it referenced
// this slot.
esp_err_t sk_auth_bond_remove(uint8_t slot);

// Enumerate all occupied bond slots. out array must hold up to
// SK_AUTH_BOND_SLOT_COUNT entries.
esp_err_t sk_auth_bond_list(sk_auth_bond_info_t out[SK_AUTH_BOND_SLOT_COUNT],
                            uint8_t *out_count);

// Look up a bond by peer_id. Returns ESP_OK + fills bond_key + slot, or
// ESP_ERR_NOT_FOUND. bond_key_out / slot_out may be NULL if not needed.
esp_err_t sk_auth_bond_lookup(const uint8_t peer_id[SK_AUTH_PEER_ID_LEN],
                              uint8_t       bond_key_out[SK_AUTH_TOKEN_LEN],
                              uint8_t      *slot_out);

// Number of occupied slots (0..SK_AUTH_BOND_SLOT_COUNT).
uint8_t sk_auth_bond_count(void);

// -- Active session ----------------------------------------------------------
//
// After sk_auth_handshake_verify_peer succeeds, the matching slot's key
// becomes "active" — every subsequent sign/verify on this connection uses
// it. Reset on session teardown.
const uint8_t *sk_auth_active_bond_key(void);    // NULL if no active session
uint8_t        sk_auth_active_bond_slot(void);   // SK_AUTH_BOND_SLOT_INVALID if none
void           sk_auth_active_bond_clear(void);  // call on session reset

// -- Pairing mode ----------------------------------------------------------
//
// Pairing mode must be opened by a physical-button trigger (sk_button) or
// an explicit `pairing.start` command over an already-trusted transport (USB).
// BLE advertising flips to "pairable" while open. State auto-closes after
// `timeout_sec` or upon successful pairing.
sk_auth_pairing_state_t sk_auth_pairing_state(void);
esp_err_t sk_auth_open_pairing_mode(uint32_t timeout_sec);
void      sk_auth_close_pairing_mode(const char *reason);

// -- ECDH (first pairing) --------------------------------------------------
//
// Flow:
//   1. Device generates its ephemeral pair via sk_auth_ecdh_begin()
//   2. Public keys are exchanged over the BLE bond (or USB)
//   3. Device calls sk_auth_ecdh_complete() to derive and persist the token
typedef struct {
    uint8_t our_public[SK_AUTH_ECDH_PUBLIC_LEN];
    uint8_t our_secret[32];   // private scalar (kept in RAM only)
} sk_auth_ecdh_ctx_t;

esp_err_t sk_auth_ecdh_begin(sk_auth_ecdh_ctx_t *ctx);

// Legacy: derive AND immediately persist as the active bond (slot 0,
// zero peer_id placeholder). Kept for callers that still don't know about
// peer_id. New code should use sk_auth_ecdh_derive + sk_auth_bond_add.
esp_err_t sk_auth_ecdh_complete(sk_auth_ecdh_ctx_t *ctx,
                                const uint8_t peer_public[SK_AUTH_ECDH_PUBLIC_LEN]);

// Derive the bond key from the ECDH shared secret without touching NVS.
// Returns the 32-byte token via out_bond_key. Caller decides when (and
// whether) to commit via sk_auth_bond_add — used by the pairing-time
// passphrase flow to keep the bond in RAM until the user proves identity.
esp_err_t sk_auth_ecdh_derive(sk_auth_ecdh_ctx_t *ctx,
                              const uint8_t peer_public[SK_AUTH_ECDH_PUBLIC_LEN],
                              uint8_t       out_bond_key[SK_AUTH_TOKEN_LEN]);

// -- Transport-agnostic pairing dispatcher ---------------------------------
//
// Single entry point for `pairing.ecdh.exchange` regardless of which
// transport the line arrived on (BLE GATT pairing characteristic, TCP
// socket pre-auth, USB CLI, ...). The caller hands in the JSON line and
// a writer for the reply; this helper:
//   1. Verifies pairing mode is currently OPEN (else writes ERR_NOT_OPEN)
//   2. Parses the cmd + peer_pub
//   3. Runs sk_auth_ecdh_begin + sk_auth_ecdh_complete (persists bond)
//   4. Writes `{"ok":true,"data":{"our_pub":"..."}}` via writer
//   5. Calls sk_auth_close_pairing_mode("ecdh_complete")
//
// Transport-specific teardown (BLE terminate / socket close) is the
// caller's job — this helper never blocks. Result tells the caller
// whether pairing succeeded so they know whether to keep the link
// open (failure → optionally retry) or tear it down (success).
typedef void (*sk_auth_pairing_writer_t)(const char *chunk, size_t len, void *user);

typedef enum {
    SK_AUTH_PAIRING_OK,            // bond persisted, peer should reconnect
    SK_AUTH_PAIRING_ERR,           // parse/ECDH failure, peer told via JSON
    SK_AUTH_PAIRING_NOT_OPEN,      // pairing window closed; peer told
} sk_auth_pairing_result_t;

sk_auth_pairing_result_t sk_auth_pairing_dispatch_line(
    const char *line,
    size_t      len,
    sk_auth_pairing_writer_t writer,
    void       *writer_user);

// -- Mutual Challenge-Response (every connection) -------------------------
//
// On each connection the transport:
//   1. Calls sk_auth_handshake_begin() to generate `our_challenge`, sends it
//   2. Awaits peer_response over the wire, passes it to verify_peer()
//   3. Receives peer_challenge and calls compute_response() to answer
//   4. Marks session authenticated if both directions verify
typedef struct {
    uint8_t our_challenge[SK_AUTH_CHALLENGE_LEN];
    int64_t started_us;
    bool    peer_verified;
} sk_auth_handshake_t;

esp_err_t sk_auth_handshake_begin(sk_auth_handshake_t *hs);

// Verify the response sent by the peer against `our_challenge`. Returns
// SK_OK on success (marks peer_verified in hs).
esp_err_t sk_auth_handshake_verify_peer(sk_auth_handshake_t *hs,
                                        const uint8_t response[SK_AUTH_RESPONSE_LEN]);

// Compute our response to the peer's challenge. Caller sends `out` on the
// wire.
esp_err_t sk_auth_handshake_answer(const uint8_t challenge[SK_AUTH_CHALLENGE_LEN],
                                   uint8_t       out[SK_AUTH_RESPONSE_LEN]);

// -- Per-message HMAC (after handshake) ------------------------------------
//
// Every CLI request must carry sig = HMAC-SHA256(token, canonical_body)[:16]
// plus a monotonic nonce and unix timestamp. The dispatcher calls this
// helper before invoking the handler.
esp_err_t sk_auth_verify_message(const char    *canonical_body,
                                 size_t         canonical_body_len,
                                 uint32_t       nonce,
                                 int64_t        ts_unix,
                                 const uint8_t  sig[SK_AUTH_HMAC_LEN]);

esp_err_t sk_auth_sign_message(const char    *canonical_body,
                               size_t         canonical_body_len,
                               uint8_t        sig_out[SK_AUTH_HMAC_LEN]);

// Reset the per-token replay window. Called by sk_secure_session_reset
// when a fresh auth handshake begins so a new SKAPP CliSigner (which
// restarts its nonce counter from 1) is not rejected as a replay of the
// previous session's nonces.
void sk_auth_replay_reset(void);

// -- Confirm tokens (critical commands) ------------------------------------
//
// Destructive commands (factory-reset, ota.fw.start, ble.unpair, ...)
// require a freshly issued, single-use confirm token. Tokens are hex
// strings of SK_AUTH_CONFIRM_TOKEN_LEN chars with a TTL, stored in RAM.
esp_err_t sk_auth_confirm_issue(char out_hex[SK_AUTH_CONFIRM_TOKEN_LEN + 1],
                                uint32_t *out_ttl_sec);
esp_err_t sk_auth_confirm_consume(const char *token);

#ifdef __cplusplus
}
#endif
