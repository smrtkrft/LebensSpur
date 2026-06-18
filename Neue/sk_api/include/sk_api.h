#pragma once

// Outbound HTTP — the device fires HTTP/HTTPS requests to external services
// (IFTTT, generic webhooks, custom REST APIs) and to paired SKAPP listeners
// (system slots, bond-signed). Each endpoint is a named, NVS-persisted
// record: URL + auth + method + headers + token (+ kind + peer_id for
// system slots).
//
// Async by design: sk_api_send() spawns a one-shot worker task; the result
// is published on the event bus as `api.sent` with a JSON payload
// `{name, ok, status, err}`. The caller does not block.
//
// TLS verification uses the ESP-IDF certificate bundle (popular CA roots
// pre-loaded — Let's Encrypt, GoDaddy, etc.). Self-signed certs need a
// per-endpoint cert config (not yet supported).
//
// -- Slot pool layout (v2 schema) -----------------------------------------
//
// Two physical buckets, distinguished by `sk_api_endpoint_t.kind`:
//
//   USER   slots [0 .. SK_API_USER_SLOTS-1]:
//     Manual webhooks the device operator configures (IFTTT, Home
//     Assistant, n8n, Shelly, generic REST). Auth comes from the stored
//     fields. Listed/edited via api.endpoint.{add,remove,list}.
//
//   SYSTEM slots [0 .. SK_API_SYSTEM_SLOTS-1]:
//     Auto-managed entries owned by paired SKAPP installations. Each
//     bound peer_id may register exactly one URL — its own HTTP listener.
//     Auth is *not* the stored token; instead the worker computes a
//     bond-keyed HMAC over the request body and emits X-SK-* headers
//     so the listener can verify the request really came from this
//     SmartKraft device. Listed/edited via api.system.{add,remove,list}.
//
// `sk_api_chain_run` fires USER slots in index order first, then SYSTEM
// slots in index order, honoring each slot's `delay_after_sec` between
// fires. Offline slots (TLS fail, connection refused) do not block the
// chain — every send is async and the loop only waits the configured
// delay between fires.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// -- Slot capacity ---------------------------------------------------------

#define SK_API_USER_SLOTS    5    // manual IoT webhooks (user-managed)
#define SK_API_SYSTEM_SLOTS  8    // paired SKAPP listeners (auto-managed,
                                  // matches SK_AUTH_BOND_SLOT_COUNT)
#define SK_API_MAX_ENDPOINTS (SK_API_USER_SLOTS + SK_API_SYSTEM_SLOTS)

#define SK_API_NAME_MAX       31
#define SK_API_URL_MAX        191
#define SK_API_TOKEN_MAX      127
#define SK_API_HEADER_MAX     31     // header name (e.g. "X-API-Key")
#define SK_API_CT_MAX         63     // Content-Type override
#define SK_API_PAYLOAD_MAX    768

// peer_id length matches sk_auth.h:SK_AUTH_PEER_ID_LEN. Hard-coded here
// so this header doesn't need to pull sk_auth.h into every translation
// unit; mismatch would be caught by a static assert in sk_api.c.
#define SK_API_PEER_ID_LEN    16

// Maximum per-endpoint sequential delay (seconds). Devices that chain
// API calls insert this many seconds of wait AFTER firing this endpoint
// before firing the next one in the sequence. Capped at 300 s so a
// misconfiguration can't lock the device out of the cooldown for too
// long.
#define SK_API_DELAY_AFTER_MAX_SEC  300

// -- Endpoint kind ---------------------------------------------------------

typedef enum {
    // Manual webhook (IoT slot). User-managed via api.endpoint.{add,remove}.
    // Auth uses the stored auth/token/header_name fields verbatim. NVS
    // schema v1 records (no kind field) are loaded as USER for backward
    // compat.
    SK_API_KIND_USER   = 0,

    // Auto-managed entry tied to a bonded SKAPP install (peer_id). Auth
    // uses a bond-keyed HMAC computed at fire time; stored token field is
    // ignored (kept zero). Cannot be edited via api.endpoint.add — only
    // via api.system.add from the matching authenticated peer.
    SK_API_KIND_SYSTEM = 1,
} sk_api_kind_t;

// -- Endpoint preset types -------------------------------------------------

typedef enum {
    // Configurable: method, auth, custom header, content-type all honoured.
    // `payload` is the request body verbatim. Most flexible — use for any
    // REST API that doesn't fit a preset.
    SK_API_GENERIC      = 0,

    // SK_API_TELEGRAM (=1) was REMOVED. Devices that need Telegram delivery
    // can configure a generic webhook pointing at any bot proxy. Numeric
    // value 1 is left vacant so existing NVS records that still contain
    // type=1 are detected and rejected by sk_api_type_str (returns
    // "unknown") rather than silently re-mapped to the next preset.
    // SK_API_TELEGRAM = 1 — RESERVED, do not reuse.

    // IFTTT Maker webhook. Hardcoded: POST, no auth header (key in URL).
    // `url` field stores event name; `token` stores Maker key. Library
    // builds `https://maker.ifttt.com/trigger/<event>/with/key/<key>`,
    // forwards `payload` as JSON body.
    SK_API_IFTTT        = 2,

    // Same flexibility as GENERIC — kept as separate label for endpoint
    // listings so cihaz operatörü "this is a webhook" niyetini görür.
    SK_API_WEBHOOK_POST = 3,
} sk_api_type_t;

// -- HTTP method ------------------------------------------------------------

typedef enum {
    SK_API_METHOD_POST   = 0,
    SK_API_METHOD_GET    = 1,
    SK_API_METHOD_PUT    = 2,
    SK_API_METHOD_DELETE = 3,
} sk_api_method_t;

// -- Auth scheme ------------------------------------------------------------

typedef enum {
    // No auth header sent. Use when key is embedded in URL (`?key=...` or
    // path token like Discord webhooks).
    SK_API_AUTH_NONE          = 0,

    // `Authorization: Bearer <token>`. Default when `token` is set and no
    // explicit auth chosen. Works for SendGrid, OpenAI, Anthropic, GitHub,
    // Stripe, etc.
    SK_API_AUTH_BEARER        = 1,

    // `Authorization: Basic <base64(token)>`. `token` must be in
    // `user:password` form. Used by Mailgun, legacy services.
    SK_API_AUTH_BASIC         = 2,

    // `<header_name>: <token>` — arbitrary auth header. Used by API Gateway
    // (`X-API-Key`), custom corporate APIs (`X-Auth-Token`), etc.
    SK_API_AUTH_CUSTOM_HEADER = 3,
} sk_api_auth_t;

// -- Trigger class ----------------------------------------------------------
//
// Which device event layer fires this endpoint. LebensSpur has two:
//   timer.alarm     — early warning (fires N times as the deadline nears)
//   timer.triggered — the final consequence (fires once at the deadline)
// A slot declares which layer(s) it belongs to so an alarm-layer event runs
// only alarm slots and a trigger-layer event runs only trigger slots.
//
// Default is TRIGGER (0) so every legacy NVS record (no `tc` key) keeps its
// existing behavior — it fires on the consequence chain, as before.
typedef enum {
    SK_API_TRIGCLASS_TRIGGER = 0,  // timer.triggered — final consequence (default)
    SK_API_TRIGCLASS_ALARM   = 1,  // timer.alarm — early-warning reminders
    SK_API_TRIGCLASS_BOTH    = 2,  // fires on either layer
} sk_api_trigclass_t;

// -- Endpoint record (read view) -------------------------------------------

typedef struct {
    char            name        [SK_API_NAME_MAX   + 1];
    sk_api_type_t   type;
    char            url         [SK_API_URL_MAX    + 1];
    char            token       [SK_API_TOKEN_MAX  + 1];
    sk_api_method_t method;
    sk_api_auth_t   auth;
    char            header_name [SK_API_HEADER_MAX + 1];   // for AUTH_CUSTOM_HEADER
    char            content_type[SK_API_CT_MAX     + 1];   // override default JSON
    uint16_t        delay_after_sec;                        // seconds to wait after
                                                            //   firing this endpoint
                                                            //   before triggering the
                                                            //   next one in sequence
    sk_api_kind_t   kind;                                   // USER vs SYSTEM
    sk_api_trigclass_t trigclass;                           // alarm vs trigger layer
    uint8_t         peer_id[SK_API_PEER_ID_LEN];            // SYSTEM only; zeroed for USER
    bool            in_use;
} sk_api_endpoint_t;

// -- Endpoint config (write — USER kind) -----------------------------------
//
// Pass to sk_api_endpoint_add. NULL/0 fields fall back to sensible defaults:
//   method       → POST
//   auth         → NONE if !token, BEARER if token set
//   content_type → "application/json"
//   header_name  → "" (irrelevant unless auth=CUSTOM_HEADER)

typedef struct {
    const char     *name;
    sk_api_type_t   type;
    const char     *url;
    const char     *token;
    sk_api_method_t method;
    sk_api_auth_t   auth;
    const char     *header_name;
    const char     *content_type;
    uint16_t        delay_after_sec;   // 0 = no wait, capped at MAX
    sk_api_trigclass_t trigclass;      // 0 = TRIGGER (default), ALARM, or BOTH
} sk_api_endpoint_cfg_t;

// -- Endpoint config (write — SYSTEM kind) ---------------------------------
//
// Used by sk_api_system_add to register a paired SKAPP listener. peer_id
// must match a bonded peer (sk_auth_bond_lookup); auth headers are emitted
// at fire time from the bond key, so no token field is exposed here.
typedef struct {
    const uint8_t *peer_id;            // 16 bytes, must reference an existing bond
    const char    *url;                // SKAPP listener URL
    uint16_t       delay_after_sec;
} sk_api_system_cfg_t;

// -- Public API ------------------------------------------------------------

// Loads endpoints from NVS, registers CLI commands, registers the
// factory-reset hook. Idempotent.
esp_err_t sk_api_init(void);

// Master switch — NVS-persisted. When disabled, sk_api_send returns
// ESP_ERR_INVALID_STATE without contacting the network.
esp_err_t sk_api_set_enabled_all(bool enabled);
bool      sk_api_is_enabled_all(void);

// Add or update a USER endpoint (manual webhook).
esp_err_t sk_api_endpoint_add(const sk_api_endpoint_cfg_t *cfg);
esp_err_t sk_api_endpoint_remove(const char *name);

// Add or update a SYSTEM endpoint (auto-managed paired SKAPP listener).
// The endpoint is keyed by peer_id, not by name; the human-visible name
// is derived deterministically from the peer_id so the SKAPP doesn't need
// to coordinate names. Returns the assigned slot (0..SYSTEM_SLOTS-1) via
// out_slot when non-NULL.
esp_err_t sk_api_system_add(const sk_api_system_cfg_t *cfg, uint8_t *out_slot);
esp_err_t sk_api_system_remove(const uint8_t peer_id[SK_API_PEER_ID_LEN]);

// Fill `out` with up to `max` configured endpoints (USER first, then
// SYSTEM, both in slot order). Returns count written.
int sk_api_endpoint_list(sk_api_endpoint_t *out, int max);

// Trigger the named endpoint asynchronously. Returns ESP_OK once the
// worker task is spawned. Result of the actual HTTP call arrives via
// event bus as `api.sent` with payload `{name, ok, status, err}`.
//
// Preconditions checked before spawning:
//   - master switch enabled (else SK_ERR_API_DISABLED)
//   - endpoint name exists  (else ESP_ERR_NOT_FOUND)
//   - WiFi STA connected    (else SK_ERR_API_OFFLINE)
esp_err_t sk_api_send(const char *name, const char *payload);

// Trigger ALL configured endpoints in (USER index order, then SYSTEM
// index order) asynchronously, as a single chain. After each endpoint
// fires, the runner waits for that endpoint's `delay_after_sec` before
// moving to the next. Used by device-side state machines (BF: when the
// focus countdown ends).
//
// Returns ESP_OK once the chain task is spawned. Per-endpoint results
// continue to publish on the event bus as `api.sent` (one event per
// step). When the whole chain finishes, an additional event is emitted:
// `api.chain.finished` with `{count, ok_count}`.
//
// Preconditions:
//   - master switch enabled (else SK_ERR_API_DISABLED)
//   - at least one endpoint configured (else ESP_ERR_NOT_FOUND)
esp_err_t sk_api_chain_run(const char *payload);

// Same as sk_api_chain_run but fires ONLY the slots whose trigger class
// matches `cls` (a slot tagged BOTH matches any class). Used by the device
// event subscribers: timer.alarm runs the ALARM class, timer.triggered runs
// the TRIGGER class. `api.chain.finished` carries the class that ran.
//
// Preconditions are the same as sk_api_chain_run, with one addition: if no
// slot matches the class, returns ESP_ERR_NOT_FOUND (nothing to fire).
esp_err_t sk_api_chain_run_class(sk_api_trigclass_t cls, const char *payload);

// -- Enum <-> string helpers (used by CLI parsing + listing) ---------------

const char *sk_api_type_str  (sk_api_type_t   type);
int         sk_api_type_from_str  (const char *s);
const char *sk_api_method_str(sk_api_method_t method);
int         sk_api_method_from_str(const char *s);
const char *sk_api_auth_str  (sk_api_auth_t   auth);
int         sk_api_auth_from_str  (const char *s);
const char *sk_api_kind_str  (sk_api_kind_t   kind);
const char *sk_api_trigclass_str(sk_api_trigclass_t cls);
int         sk_api_trigclass_from_str(const char *s);

#ifdef __cplusplus
}
#endif
