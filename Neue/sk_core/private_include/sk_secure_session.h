#pragma once

// Connection-level Mutual Challenge-Response state machine. Shared by the
// BLE GATT and TCP NDJSON transports — both sit on top of NDJSON line
// framing, so the session works in lines.
//
// Wire protocol:
//   device → peer : {"evt":"auth.challenge","data":"<32hex>"}
//   peer  → device: {"cmd":"auth.response","args":{"response":"<32hex>","challenge":"<32hex>"}}
//   device → peer : {"ok":true,"data":{"answer":"<32hex>"}}
//
// Caller responsibilities:
//   1. Construct a sk_secure_session_t per connection (zero-init).
//   2. Call sk_secure_session_begin() once the link is up — this sends our
//      challenge to the peer.
//   3. For every line received from the peer, call sk_secure_session_feed_line().
//      Look at the return value to decide whether to drop the connection or
//      pass the line on to sk_cli_dispatch_line().
//   4. Call sk_secure_session_reset() on disconnect.
//   5. Optionally poll sk_secure_session_timed_out() to drop stalled handshakes.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#include "sk_auth.h"
#include "sk_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SK_SESSION_HANDSHAKE_TIMEOUT_US  (5LL * 1000 * 1000)  // 5 s

typedef enum {
    SK_SESSION_FRESH = 0,             // zero-initialized, begin() not yet called
    SK_SESSION_AWAITING_PEER,         // we sent challenge; waiting on peer
    SK_SESSION_AUTHENTICATED,
    SK_SESSION_FAILED,
} sk_secure_session_state_t;

// Sender callback. `chunk` is one complete NDJSON line (including trailing
// '\n'). The transport is responsible for any extra framing (BLE length
// prefix, etc.) — the session itself only emits lines.
typedef void (*sk_session_send_fn)(const char *chunk, size_t len, void *user);

typedef struct {
    sk_secure_session_state_t state;
    sk_auth_handshake_t       hs;
    sk_session_send_fn        send;
    void                     *send_user;

    // Content-access passphrase gate. After mutual C-R succeeds the session
    // is AUTHENTICATED but `passphrase_unlocked` may still be false: the
    // peer must send a successful `auth.passphrase.verify` before any
    // non-verify command is accepted by sk_secure_session_dispatch_signed.
    //
    // Set to true automatically when:
    //   - no passphrase configured, OR
    //   - configured but `always_required` mode is off (pairing-time only).
    //
    // USB transport never reaches this struct so it is bypassed by
    // construction; sk_transport_usb dispatches directly through sk_cli.
    bool passphrase_unlocked;
} sk_secure_session_t;

// Outcome of sk_secure_session_feed_line().
typedef enum {
    SK_SESSION_FEED_AUTH_PROGRESSED,  // line was an auth message; session advanced
    SK_SESSION_FEED_PASSTHROUGH,      // not auth; if authed → dispatch to sk_cli; else → reject
    SK_SESSION_FEED_AUTH_INVALID,     // protocol violation — caller should drop the connection
} sk_session_feed_t;

// Begin a session: generate our challenge, send it via `send`, advance state
// to AWAITING_PEER. Fails if no bond exists (peer must pair first).
esp_err_t sk_secure_session_begin(sk_secure_session_t *s,
                                  sk_session_send_fn   send,
                                  void                *send_user);

// Feed one complete NDJSON line received from the peer.
sk_session_feed_t sk_secure_session_feed_line(sk_secure_session_t *s,
                                              const char           *line);

// True once both sides have verified each other.
static inline bool sk_secure_session_authed(const sk_secure_session_t *s)
{
    return s && s->state == SK_SESSION_AUTHENTICATED;
}

// True if begin() was called but AUTHENTICATED was not reached within
// SK_SESSION_HANDSHAKE_TIMEOUT_US. Caller polls this periodically and
// drops the connection when it returns true.
bool sk_secure_session_timed_out(const sk_secure_session_t *s);

// Reset to FRESH (call on disconnect).
void sk_secure_session_reset(sk_secure_session_t *s);

// -- Per-message authentication envelope -------------------------------------
//
// After connection-level mutual C-R completes (state == AUTHENTICATED), every
// command line must arrive as a signed envelope:
//   {"body":"<inner json>","sig":"<32hex>","nonce":<u32>,"ts":<i64>}
//
// `body` is the regular CLI machine-mode command (cmd/args/id/...). It is
// HMAC'd as a raw byte string, so the peer doesn't need to canonicalize JSON.
// `nonce` is monotonic, replay-protected via a 64-slot sliding window. `ts`
// is currently advisory (cihazda SNTP yok); accepted as 0 too.
//
// Verifies the envelope, then dispatches the inner body through sk_cli on
// success. On failure emits an ERR_HMAC_INVALID response via writer and
// returns without dispatching. Used by BLE GATT and TCP transports after the
// session reaches AUTHENTICATED.
//
// The session pointer is consulted for the passphrase gate: while
// `s->passphrase_unlocked` is false, only `auth.passphrase.verify` is
// admitted (it is intercepted here and mutates the gate on success);
// every other command is rejected with ERR_SESSION_LOCKED. Callers that
// don't care about the gate (legacy paths) may pass NULL — the gate is
// then implicitly considered unlocked.
void sk_secure_session_dispatch_signed(sk_secure_session_t *s,
                                       const char          *line,
                                       sk_cli_writer_t      writer,
                                       void                *user);

#ifdef __cplusplus
}
#endif
