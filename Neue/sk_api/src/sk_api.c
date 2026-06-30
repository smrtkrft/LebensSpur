// Outbound HTTP. Two slot buckets:
//   USER   — manual webhooks (IFTTT, Home Assistant, n8n, Shelly), auth
//            from stored token/header fields.
//   SYSTEM — auto-managed entries owned by paired SKAPP installs. Auth
//            is a bond-keyed HMAC (SHA-256/16) injected at fire time so
//            the SKAPP listener can prove the request came from this
//            SmartKraft device. Stored token field is unused.
//
// Each send() spawns a short-lived worker task that builds the
// type-specific URL/headers/body, performs the request via
// esp_http_client (TLS via cert bundle), publishes "api.sent" with
// success/failure detail, then exits.
//
// USER auth schemes: NONE, BEARER, BASIC, CUSTOM_HEADER.
// SYSTEM auth scheme: bond-HMAC, fixed.
//
// Methods: POST (default), GET, PUT, DELETE.
//
// Storage / access control (Faz C, opsiyon C — hibrit, 2026-05-14):
//   Storage is still plaintext NVS (`sk_api` namespace). Every api.*
//   CLI command is marked requires_auth=true so the dispatcher rejects
//   USB CLI invocations with ERR_NOT_AUTHENTICATED — webhook URLs and
//   auth tokens cannot be read or written through the unauthenticated
//   USB path. BLE/TCP after the HMAC-envelope handshake remain the
//   only channel that can list/add/remove endpoints.
//
//   Full migration to bf_secure_store's encrypted 100 KB userdata blob
//   (with 50+ slot capacity) is deferred; the current 5 USER + 8 SYSTEM
//   slot budget is comfortable and an in-place blob migration would
//   pull in a one-shot NVS-->blob copier plus an `api.migrated` flag
//   for ~250-400 lines of churn with no immediate user-visible win.
//   Re-open when slot pressure or a flash-dump threat model warrants
//   it (memory: project_pending_phases.md "Faz C").

#include "sk_api.h"
#include "sk_auth.h"
#include "sk_capabilities.h"
#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"
#include "sk_identity.h"
#include "sk_log.h"
#include "sk_wifi.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "sk_api";

// Compile-time guard: the public peer_id length must match the auth
// component's. Both should equal 16. If sk_auth ever bumps this we want
// a clean build break, not silent buffer truncation.
_Static_assert(SK_API_PEER_ID_LEN == SK_AUTH_PEER_ID_LEN,
               "sk_api peer_id length must match sk_auth");

#define NVS_NS          "sk_api"
#define NVS_NS_GLOBAL   "sk_api_g"
#define NVS_KEY_MASTER  "all_enabled"
#define WORKER_STACK    8192       // TLS handshake needs ~8 KB
#define HTTP_TIMEOUT_MS 3000   // Faz 1: 10000'den indirildi; Faz 2 retry telafi eder.
#define DEFAULT_CT      "application/json"

// Storage layout:
//   s_user[0..SK_API_USER_SLOTS-1]    — physical USER slots
//   s_system[0..SK_API_SYSTEM_SLOTS-1] — physical SYSTEM slots
// All firing / lookup helpers iterate USER first, then SYSTEM, so chain
// order matches the user model: "5 manuel slot once, then paired
// computers."
static sk_api_endpoint_t s_user  [SK_API_USER_SLOTS];
static sk_api_endpoint_t s_system[SK_API_SYSTEM_SLOTS];
static bool              s_initialized = false;

// -- Enum <-> string maps --------------------------------------------------

static const struct { sk_api_type_t t; const char *s; } TYPES[] = {
    { SK_API_GENERIC,      "generic"      },
    // telegram preset REMOVED — use generic/webhook_post against a bot proxy
    // if needed. Slot 1 left vacant so legacy NVS records with type=1 are
    // surfaced as "unknown" rather than silently misinterpreted.
    { SK_API_IFTTT,        "ifttt"        },
    { SK_API_WEBHOOK_POST, "webhook_post" },
};

static const struct { sk_api_method_t m; const char *s; esp_http_client_method_t esp; } METHODS[] = {
    { SK_API_METHOD_POST,   "POST",   HTTP_METHOD_POST   },
    { SK_API_METHOD_GET,    "GET",    HTTP_METHOD_GET    },
    { SK_API_METHOD_PUT,    "PUT",    HTTP_METHOD_PUT    },
    { SK_API_METHOD_DELETE, "DELETE", HTTP_METHOD_DELETE },
};

static const struct { sk_api_auth_t a; const char *s; } AUTHS[] = {
    { SK_API_AUTH_NONE,          "none"          },
    { SK_API_AUTH_BEARER,        "bearer"        },
    { SK_API_AUTH_BASIC,         "basic"         },
    { SK_API_AUTH_CUSTOM_HEADER, "custom_header" },
};

static const struct { sk_api_kind_t k; const char *s; } KINDS[] = {
    { SK_API_KIND_USER,   "user"   },
    { SK_API_KIND_SYSTEM, "system" },
};

static const struct { sk_api_trigclass_t c; const char *s; } TRIGCLASSES[] = {
    { SK_API_TRIGCLASS_TRIGGER, "trigger" },
    { SK_API_TRIGCLASS_ALARM,   "alarm"   },
    { SK_API_TRIGCLASS_BOTH,    "both"    },
};

const char *sk_api_type_str(sk_api_type_t type)
{
    for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (TYPES[i].t == type) return TYPES[i].s;
    }
    return "unknown";
}

int sk_api_type_from_str(const char *s)
{
    if (!s) return -1;
    for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (strcasecmp(s, TYPES[i].s) == 0) return (int)TYPES[i].t;
    }
    return -1;
}

const char *sk_api_method_str(sk_api_method_t method)
{
    for (size_t i = 0; i < sizeof(METHODS) / sizeof(METHODS[0]); i++) {
        if (METHODS[i].m == method) return METHODS[i].s;
    }
    return "POST";
}

int sk_api_method_from_str(const char *s)
{
    if (!s) return -1;
    for (size_t i = 0; i < sizeof(METHODS) / sizeof(METHODS[0]); i++) {
        if (strcasecmp(s, METHODS[i].s) == 0) return (int)METHODS[i].m;
    }
    return -1;
}

static esp_http_client_method_t method_to_esp(sk_api_method_t m)
{
    for (size_t i = 0; i < sizeof(METHODS) / sizeof(METHODS[0]); i++) {
        if (METHODS[i].m == m) return METHODS[i].esp;
    }
    return HTTP_METHOD_POST;
}

const char *sk_api_auth_str(sk_api_auth_t auth)
{
    for (size_t i = 0; i < sizeof(AUTHS) / sizeof(AUTHS[0]); i++) {
        if (AUTHS[i].a == auth) return AUTHS[i].s;
    }
    return "none";
}

int sk_api_auth_from_str(const char *s)
{
    if (!s) return -1;
    // Accept "header" as shorthand for "custom_header"
    if (strcasecmp(s, "header") == 0) return (int)SK_API_AUTH_CUSTOM_HEADER;
    for (size_t i = 0; i < sizeof(AUTHS) / sizeof(AUTHS[0]); i++) {
        if (strcasecmp(s, AUTHS[i].s) == 0) return (int)AUTHS[i].a;
    }
    return -1;
}

const char *sk_api_kind_str(sk_api_kind_t kind)
{
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        if (KINDS[i].k == kind) return KINDS[i].s;
    }
    return "user";
}

const char *sk_api_trigclass_str(sk_api_trigclass_t cls)
{
    for (size_t i = 0; i < sizeof(TRIGCLASSES) / sizeof(TRIGCLASSES[0]); i++) {
        if (TRIGCLASSES[i].c == cls) return TRIGCLASSES[i].s;
    }
    return "trigger";
}

int sk_api_trigclass_from_str(const char *s)
{
    if (!s) return -1;
    for (size_t i = 0; i < sizeof(TRIGCLASSES) / sizeof(TRIGCLASSES[0]); i++) {
        if (strcasecmp(s, TRIGCLASSES[i].s) == 0) return (int)TRIGCLASSES[i].c;
    }
    return -1;
}

// True if a slot tagged `slot_tc` should fire for an event of class `want`.
// A slot tagged BOTH matches any class.
static bool trigclass_matches(sk_api_trigclass_t slot_tc, sk_api_trigclass_t want)
{
    if (slot_tc == SK_API_TRIGCLASS_BOTH) return true;
    return slot_tc == want;
}

// -- Hex helpers -----------------------------------------------------------

static const char HEX[] = "0123456789abcdef";

static void bytes_to_hex(const uint8_t *bytes, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = HEX[(bytes[i] >> 4) & 0xF];
        out[2 * i + 1] = HEX[ bytes[i]       & 0xF];
    }
    out[2 * n] = '\0';
}

static bool peer_id_is_zero(const uint8_t peer_id[SK_API_PEER_ID_LEN])
{
    for (size_t i = 0; i < SK_API_PEER_ID_LEN; i++) {
        if (peer_id[i] != 0) return false;
    }
    return true;
}

// -- NVS persistence -------------------------------------------------------
//
// Schema v2: same per-slot keys as v1 plus two extra:
//   n<slot>kind  (u8)  — 0=USER, 1=SYSTEM. Missing → 0 (USER, legacy).
//   n<slot>pid   (blob, 16 B) — SYSTEM only. Missing → all zeros (USER).
//
// Slots are addressed by a single linear index across both buckets:
//   linear = slot               (USER, 0..USER_SLOTS-1)
//   linear = USER_SLOTS + slot  (SYSTEM, 0..SYSTEM_SLOTS-1)
// This keeps the existing key namespace intact for migration.

static int linear_index(sk_api_kind_t kind, int slot)
{
    if (kind == SK_API_KIND_USER)   return slot;
    return SK_API_USER_SLOTS + slot;
}

static sk_api_endpoint_t *slot_ptr(sk_api_kind_t kind, int slot)
{
    if (kind == SK_API_KIND_USER) {
        if (slot < 0 || slot >= SK_API_USER_SLOTS) return NULL;
        return &s_user[slot];
    }
    if (slot < 0 || slot >= SK_API_SYSTEM_SLOTS) return NULL;
    return &s_system[slot];
}

static void slot_keys(int linear,
                      char k_name[16], char k_url[16], char k_tok[16],
                      char k_type[16], char k_meth[16], char k_auth[16],
                      char k_hdr[16],  char k_ct[16],   char k_dly[16],
                      char k_kind[16], char k_pid[16])
{
    // %hhu + (unsigned char) cast bounds linear to <= 3 digits, so GCC
    // format-truncation analysis is happy with the 16-byte NVS-key buffers.
    snprintf(k_name, 16, "n%hhuname", (unsigned char)linear);
    snprintf(k_url,  16, "n%hhuurl",  (unsigned char)linear);
    snprintf(k_tok,  16, "n%hhutok",  (unsigned char)linear);
    snprintf(k_type, 16, "n%hhutype", (unsigned char)linear);
    snprintf(k_meth, 16, "n%hhumeth", (unsigned char)linear);
    snprintf(k_auth, 16, "n%hhuauth", (unsigned char)linear);
    snprintf(k_hdr,  16, "n%hhuhdr",  (unsigned char)linear);
    snprintf(k_ct,   16, "n%hhuct",   (unsigned char)linear);
    snprintf(k_dly,  16, "n%hhudly",  (unsigned char)linear);
    snprintf(k_kind, 16, "n%hhukind", (unsigned char)linear);
    snprintf(k_pid,  16, "n%hhupid",  (unsigned char)linear);
}

static void save_linear(int linear, const sk_api_endpoint_t *e)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k_name[16], k_url[16], k_tok[16], k_type[16], k_meth[16],
         k_auth[16], k_hdr[16], k_ct[16], k_dly[16], k_kind[16], k_pid[16];
    slot_keys(linear, k_name, k_url, k_tok, k_type, k_meth, k_auth,
              k_hdr, k_ct, k_dly, k_kind, k_pid);
    // Schema v3: trigger class. Built inline to avoid threading a 12th
    // param through slot_keys; same "n<linear>" key namespace.
    char k_tc[16];
    snprintf(k_tc, sizeof(k_tc), "n%hhutc", (unsigned char)linear);
    if (e->in_use) {
        nvs_set_str(h, k_name, e->name);
        nvs_set_str(h, k_url,  e->url);
        nvs_set_str(h, k_tok,  e->token);
        nvs_set_str(h, k_hdr,  e->header_name);
        nvs_set_str(h, k_ct,   e->content_type);
        nvs_set_u8 (h, k_type, (uint8_t)e->type);
        nvs_set_u8 (h, k_meth, (uint8_t)e->method);
        nvs_set_u8 (h, k_auth, (uint8_t)e->auth);
        nvs_set_u16(h, k_dly,  e->delay_after_sec);
        nvs_set_u8 (h, k_kind, (uint8_t)e->kind);
        nvs_set_u8 (h, k_tc,   (uint8_t)e->trigclass);
        if (e->kind == SK_API_KIND_SYSTEM) {
            nvs_set_blob(h, k_pid, e->peer_id, SK_API_PEER_ID_LEN);
        } else {
            nvs_erase_key(h, k_pid);
        }
    } else {
        nvs_erase_key(h, k_name); nvs_erase_key(h, k_url);
        nvs_erase_key(h, k_tok);  nvs_erase_key(h, k_type);
        nvs_erase_key(h, k_meth); nvs_erase_key(h, k_auth);
        nvs_erase_key(h, k_hdr);  nvs_erase_key(h, k_ct);
        nvs_erase_key(h, k_dly);  nvs_erase_key(h, k_kind);
        nvs_erase_key(h, k_pid);  nvs_erase_key(h, k_tc);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void save_slot(sk_api_kind_t kind, int slot)
{
    sk_api_endpoint_t *e = slot_ptr(kind, slot);
    if (!e) return;
    save_linear(linear_index(kind, slot), e);
}

// Loads NVS into RAM. Handles v1 (no kind/pid keys) by treating those
// records as USER slots — they're indexed in the lower [0..USER_SLOTS-1]
// linear range so the bucket assignment is identical to old behavior.
// Slots in the SYSTEM range that lack kind=1 are wiped on first save.
static void load_all_slots(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    for (int linear = 0; linear < SK_API_MAX_ENDPOINTS; linear++) {
        char k_name[16], k_url[16], k_tok[16], k_type[16], k_meth[16],
             k_auth[16], k_hdr[16], k_ct[16], k_dly[16], k_kind[16], k_pid[16];
        slot_keys(linear, k_name, k_url, k_tok, k_type, k_meth, k_auth,
                  k_hdr, k_ct, k_dly, k_kind, k_pid);

        sk_api_endpoint_t scratch = {0};
        size_t name_sz = sizeof(scratch.name);
        if (nvs_get_str(h, k_name, scratch.name, &name_sz) != ESP_OK) continue;

        size_t url_sz = sizeof(scratch.url);
        nvs_get_str(h, k_url, scratch.url, &url_sz);
        size_t tok_sz = sizeof(scratch.token);
        nvs_get_str(h, k_tok, scratch.token, &tok_sz);
        size_t hdr_sz = sizeof(scratch.header_name);
        nvs_get_str(h, k_hdr, scratch.header_name, &hdr_sz);
        size_t ct_sz = sizeof(scratch.content_type);
        nvs_get_str(h, k_ct, scratch.content_type, &ct_sz);

        uint8_t v = 0;
        nvs_get_u8(h, k_type, &v); scratch.type   = (sk_api_type_t)v;
        v = 0;
        nvs_get_u8(h, k_meth, &v); scratch.method = (sk_api_method_t)v;
        v = 0;
        nvs_get_u8(h, k_auth, &v); scratch.auth   = (sk_api_auth_t)v;

        scratch.delay_after_sec = 0;
        nvs_get_u16(h, k_dly, &scratch.delay_after_sec);

        v = SK_API_KIND_USER;
        nvs_get_u8(h, k_kind, &v); scratch.kind = (sk_api_kind_t)v;

        // Schema v3: trigger class. Missing key → TRIGGER (legacy default).
        char k_tc[16];
        snprintf(k_tc, sizeof(k_tc), "n%hhutc", (unsigned char)linear);
        v = SK_API_TRIGCLASS_TRIGGER;
        nvs_get_u8(h, k_tc, &v); scratch.trigclass = (sk_api_trigclass_t)v;

        size_t pid_sz = SK_API_PEER_ID_LEN;
        if (scratch.kind == SK_API_KIND_SYSTEM) {
            esp_err_t pe = nvs_get_blob(h, k_pid, scratch.peer_id, &pid_sz);
            if (pe != ESP_OK || pid_sz != SK_API_PEER_ID_LEN) {
                // SYSTEM record missing peer_id is corrupt — skip the
                // load so a fresh registration can claim the slot.
                continue;
            }
        }

        if (scratch.content_type[0] == '\0') {
            strncpy(scratch.content_type, DEFAULT_CT,
                    sizeof(scratch.content_type) - 1);
        }
        scratch.in_use = true;

        // Place into the correct in-RAM bucket based on linear index.
        // Bucket boundary: < USER_SLOTS → user; >= USER_SLOTS → system.
        // If the persisted kind disagrees with the linear bucket (legacy
        // NVS has linear>=USER_SLOTS but no kind=SYSTEM key) we trust the
        // persisted kind and re-locate in RAM, skipping the original
        // NVS entry — it'll be cleaned up on next save.
        if (scratch.kind == SK_API_KIND_USER) {
            int target = linear < SK_API_USER_SLOTS ? linear : -1;
            if (target >= 0) s_user[target] = scratch;
        } else if (scratch.kind == SK_API_KIND_SYSTEM) {
            int target = linear >= SK_API_USER_SLOTS
                           ? (linear - SK_API_USER_SLOTS) : -1;
            if (target >= 0) s_system[target] = scratch;
        }
    }
    nvs_close(h);
}

// Lookup helpers — by name OR by peer_id. USER slots are name-keyed,
// SYSTEM slots are peer_id-keyed (the human-visible name is just a
// derived label for display).

static int find_user_slot_by_name(const char *name)
{
    for (int i = 0; i < SK_API_USER_SLOTS; i++) {
        if (s_user[i].in_use && strcmp(s_user[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_system_slot_by_name(const char *name)
{
    for (int i = 0; i < SK_API_SYSTEM_SLOTS; i++) {
        if (s_system[i].in_use && strcmp(s_system[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_system_slot_by_peer(const uint8_t peer_id[SK_API_PEER_ID_LEN])
{
    for (int i = 0; i < SK_API_SYSTEM_SLOTS; i++) {
        if (!s_system[i].in_use) continue;
        if (memcmp(s_system[i].peer_id, peer_id, SK_API_PEER_ID_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_user_slot(void)
{
    for (int i = 0; i < SK_API_USER_SLOTS; i++) {
        if (!s_user[i].in_use) return i;
    }
    return -1;
}

static int find_free_system_slot(void)
{
    for (int i = 0; i < SK_API_SYSTEM_SLOTS; i++) {
        if (!s_system[i].in_use) return i;
    }
    return -1;
}

// Look up an endpoint by name across both buckets; preserves the kind so
// callers can distinguish USER vs SYSTEM. Returns NULL if not found.
static sk_api_endpoint_t *find_any_by_name(const char *name)
{
    int s = find_user_slot_by_name(name);
    if (s >= 0) return &s_user[s];
    s = find_system_slot_by_name(name);
    if (s >= 0) return &s_system[s];
    return NULL;
}

// -- Public API ------------------------------------------------------------

esp_err_t sk_api_set_enabled_all(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_GLOBAL, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_MASTER, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

bool sk_api_is_enabled_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_GLOBAL, NVS_READONLY, &h) != ESP_OK) return true;
    uint8_t en = 1;
    nvs_get_u8(h, NVS_KEY_MASTER, &en);
    nvs_close(h);
    return en != 0;
}

esp_err_t sk_api_endpoint_add(const sk_api_endpoint_cfg_t *cfg)
{
    if (!cfg || !cfg->name || !cfg->url) return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->name) > SK_API_NAME_MAX) return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->url)  > SK_API_URL_MAX)  return ESP_ERR_INVALID_ARG;
    if (cfg->token       && strlen(cfg->token)        > SK_API_TOKEN_MAX)  return ESP_ERR_INVALID_ARG;
    if (cfg->header_name && strlen(cfg->header_name)  > SK_API_HEADER_MAX) return ESP_ERR_INVALID_ARG;
    if (cfg->content_type&& strlen(cfg->content_type) > SK_API_CT_MAX)     return ESP_ERR_INVALID_ARG;

    // USER kind name conflict guard: a SYSTEM slot may already occupy the
    // same human-visible name (deterministic skapp-<peer> label). Reject
    // so the user understands the slot is reserved.
    if (find_system_slot_by_name(cfg->name) >= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Preset-type guards: catch missing required fields at add time
    // instead of waiting until the first send fails.
    if (cfg->type == SK_API_IFTTT) {
        if (!cfg->token || !cfg->token[0]) return ESP_ERR_INVALID_ARG;
    }
    if (cfg->auth == SK_API_AUTH_CUSTOM_HEADER) {
        if (!cfg->header_name || !cfg->header_name[0]) return ESP_ERR_INVALID_ARG;
        if (!cfg->token       || !cfg->token[0])       return ESP_ERR_INVALID_ARG;
    }

    int slot = find_user_slot_by_name(cfg->name);
    if (slot < 0) slot = find_free_user_slot();
    if (slot < 0) return ESP_ERR_NO_MEM;

    sk_api_endpoint_t *e = &s_user[slot];
    memset(e, 0, sizeof(*e));
    strcpy(e->name, cfg->name);
    strcpy(e->url,  cfg->url);
    if (cfg->token)        strcpy(e->token,        cfg->token);
    if (cfg->header_name)  strcpy(e->header_name,  cfg->header_name);
    if (cfg->content_type) strcpy(e->content_type, cfg->content_type);
    if (e->content_type[0] == '\0') strcpy(e->content_type, DEFAULT_CT);
    e->type   = cfg->type;
    e->method = cfg->method;   // 0 = POST default
    e->auth   = cfg->auth;     // 0 = NONE; promoted to BEARER below if token
    e->delay_after_sec = cfg->delay_after_sec;
    if (e->delay_after_sec > SK_API_DELAY_AFTER_MAX_SEC) {
        e->delay_after_sec = SK_API_DELAY_AFTER_MAX_SEC;
    }
    e->kind = SK_API_KIND_USER;
    // Trigger class: clamp out-of-range values to TRIGGER (the default).
    e->trigclass = (cfg->trigclass == SK_API_TRIGCLASS_ALARM ||
                    cfg->trigclass == SK_API_TRIGCLASS_BOTH)
                       ? cfg->trigclass : SK_API_TRIGCLASS_TRIGGER;
    memset(e->peer_id, 0, sizeof(e->peer_id));

    // Sensible auth default: token without explicit auth → Bearer.
    if (e->auth == SK_API_AUTH_NONE && e->token[0]) {
        e->auth = SK_API_AUTH_BEARER;
    }

    e->in_use = true;
    save_slot(SK_API_KIND_USER, slot);
    return ESP_OK;
}

esp_err_t sk_api_endpoint_remove(const char *name)
{
    int slot = find_user_slot_by_name(name);
    if (slot < 0) return ESP_ERR_NOT_FOUND;
    memset(&s_user[slot], 0, sizeof(s_user[slot]));
    save_slot(SK_API_KIND_USER, slot);
    return ESP_OK;
}

esp_err_t sk_api_system_add(const sk_api_system_cfg_t *cfg, uint8_t *out_slot)
{
    if (!cfg || !cfg->peer_id || !cfg->url) return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->url) > SK_API_URL_MAX)  return ESP_ERR_INVALID_ARG;
    if (peer_id_is_zero(cfg->peer_id))      return ESP_ERR_INVALID_ARG;

    // Caller must already be a bonded peer; double-check via auth.
    uint8_t bond_slot = SK_AUTH_BOND_SLOT_INVALID;
    if (sk_auth_bond_lookup(cfg->peer_id, NULL, &bond_slot) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    int slot = find_system_slot_by_peer(cfg->peer_id);
    if (slot < 0) slot = find_free_system_slot();
    if (slot < 0) return ESP_ERR_NO_MEM;

    sk_api_endpoint_t *e = &s_system[slot];
    memset(e, 0, sizeof(*e));
    // Deterministic display name: skapp-<first 8 hex of peer_id>. No PII
    // and stable across re-registrations from the same peer.
    char pidhex[SK_API_PEER_ID_LEN * 2 + 1];
    bytes_to_hex(cfg->peer_id, SK_API_PEER_ID_LEN, pidhex);
    snprintf(e->name, sizeof(e->name), "skapp-%.8s", pidhex);
    strcpy(e->url, cfg->url);
    e->type   = SK_API_GENERIC;
    e->method = SK_API_METHOD_POST;
    e->auth   = SK_API_AUTH_NONE;     // bond-HMAC injected at fire time
    strcpy(e->content_type, DEFAULT_CT);
    e->delay_after_sec = cfg->delay_after_sec;
    if (e->delay_after_sec > SK_API_DELAY_AFTER_MAX_SEC) {
        e->delay_after_sec = SK_API_DELAY_AFTER_MAX_SEC;
    }
    e->kind = SK_API_KIND_SYSTEM;
    memcpy(e->peer_id, cfg->peer_id, SK_API_PEER_ID_LEN);
    e->in_use = true;
    save_slot(SK_API_KIND_SYSTEM, slot);

    if (out_slot) *out_slot = (uint8_t)slot;
    return ESP_OK;
}

esp_err_t sk_api_system_remove(const uint8_t peer_id[SK_API_PEER_ID_LEN])
{
    if (!peer_id) return ESP_ERR_INVALID_ARG;
    int slot = find_system_slot_by_peer(peer_id);
    if (slot < 0) return ESP_ERR_NOT_FOUND;
    memset(&s_system[slot], 0, sizeof(s_system[slot]));
    save_slot(SK_API_KIND_SYSTEM, slot);
    return ESP_OK;
}

esp_err_t sk_api_system_remove_all(void)
{
    int cleared = 0;
    for (int i = 0; i < SK_API_SYSTEM_SLOTS; i++) {
        if (!s_system[i].in_use) continue;
        memset(&s_system[i], 0, sizeof(s_system[i]));
        save_slot(SK_API_KIND_SYSTEM, i);
        cleared++;
    }
    ESP_LOGW(TAG, "system_remove_all: cleared %d slot(s)", cleared);
    return ESP_OK;
}

int sk_api_endpoint_list(sk_api_endpoint_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < SK_API_USER_SLOTS && n < max; i++) {
        if (s_user[i].in_use) out[n++] = s_user[i];
    }
    for (int i = 0; i < SK_API_SYSTEM_SLOTS && n < max; i++) {
        if (s_system[i].in_use) out[n++] = s_system[i];
    }
    return n;
}

// -- HTTP send worker ------------------------------------------------------

typedef struct {
    sk_api_endpoint_t ep;
    char             *payload;
} send_job_t;

static sk_err_t map_http_err(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_HTTP_CONNECT:           return SK_ERR_API_CONNECT;
    case ESP_ERR_HTTP_FETCH_HEADER:      return SK_ERR_API_CONNECT;
    case ESP_ERR_HTTP_INVALID_TRANSPORT: return SK_ERR_API_TLS;
    case ESP_ERR_HTTP_CONNECTING:        return SK_ERR_API_CONNECT;
    case ESP_ERR_HTTP_EAGAIN:            return SK_ERR_API_TIMEOUT;
    case ESP_ERR_TIMEOUT:                return SK_ERR_API_TIMEOUT;
    default:                             return SK_ERR_API_CONNECT;
    }
}

static void worker_publish_result(const char *name, bool ok, int status, sk_err_t err)
{
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"ok\":%s,\"status\":%d,\"err\":\"%s\"}",
             name, ok ? "true" : "false", status,
             err == SK_OK ? "" : sk_err_code_string(err));
    sk_event_bus_publish("api.sent", payload);
}

// Bond-keyed HMAC-SHA256 over (body || ":" || timestamp || ":" || nonce_hex).
// Output is full 32-byte digest; caller hex-encodes the truncated 16 bytes
// it actually emits so the wire string stays short.
static int hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *msg, size_t msg_len,
                       uint8_t out[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, md, 1 /* HMAC */);
    if (rc != 0) goto done;
    rc = mbedtls_md_hmac_starts(&ctx, key, key_len);
    if (rc != 0) goto done;
    rc = mbedtls_md_hmac_update(&ctx, msg, msg_len);
    if (rc != 0) goto done;
    rc = mbedtls_md_hmac_finish(&ctx, out);
done:
    mbedtls_md_free(&ctx);
    return rc;
}

// Build the canonical message to sign (güvenlik.md Madde 18):
//   `<body_len> || "\n" || body || "\n" || ts || "\n" || nonce_hex`
// The leading decimal byte-length prefix makes the framing unambiguous even
// if the body contains `\n<digits>\n<hex>` look-alikes.
//
// The listener side (SKAPP webhook_receiver.dart) reproduces this string from
// the same fields and rejects any request whose computed HMAC differs from
// the X-SK-Signature header, so the canonical form must be byte-identical on
// both sides. NOTE: lockstep firmware+app break — a device on old firmware
// fails verification against a new listener until reflashed.
static int build_sign_msg(const char *body, size_t body_len,
                          const char *ts_str, const char *nonce_hex,
                          uint8_t *out, size_t cap)
{
    int n = snprintf((char *)out, cap, "%zu\n%.*s\n%s\n%s",
                     body_len,
                     (int)body_len, body ? body : "",
                     ts_str, nonce_hex);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

// Resolve bond key for a SYSTEM endpoint and emit the X-SK-* headers on
// the http client handle. Returns SK_OK or a specific error.
static sk_err_t apply_bond_signature(esp_http_client_handle_t c,
                                     const sk_api_endpoint_t *ep,
                                     const char *body)
{
    uint8_t bond_key[SK_AUTH_TOKEN_LEN];
    if (sk_auth_bond_lookup(ep->peer_id, bond_key, NULL) != ESP_OK) {
        return SK_ERR_API_NOT_CONFIGURED;
    }

    // Timestamp: unix seconds. Listener checks ±60s window to tolerate
    // clock drift while keeping replay risk small.
    char ts_str[24];
    time_t now = time(NULL);
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)now);

    // Nonce: 16 bytes from esp_random (CSPRNG). Hex-encoded, 32 chars.
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    char nonce_hex[33];
    bytes_to_hex(nonce, sizeof(nonce), nonce_hex);

    // Build canonical message + sign over the EXACT bytes we ship as
    // the HTTP body — never truncate independently here. send_worker's
    // body buffer is 1024 bytes; allow that plus 80 chars of separator
    // slop so the signature always covers what the listener receives.
    // Mismatch between signed and sent length would cause silent 401s
    // in the receiver and is not worth the few stack bytes saved.
    size_t body_len = body ? strlen(body) : 0;
    uint8_t  sign_msg[1024 + 80];
    int      msg_len = build_sign_msg(body, body_len, ts_str, nonce_hex,
                                      sign_msg, sizeof(sign_msg));
    if (msg_len < 0) {
        // Defensive: payload too large for buffer. Treat as not configured
        // so the caller publishes a clear api.sent failure event.
        memset(bond_key, 0, sizeof(bond_key));
        return SK_ERR_API_NOT_CONFIGURED;
    }

    uint8_t mac[32];
    int rc = hmac_sha256(bond_key, sizeof(bond_key), sign_msg, (size_t)msg_len, mac);
    // Wipe the bond key from the local stack frame ASAP.
    memset(bond_key, 0, sizeof(bond_key));
    if (rc != 0) return SK_ERR_INTERNAL;

    // Emit truncated MAC (16 bytes / 32 hex chars) — same length the
    // existing CLI HMAC envelope uses, plenty against forgery and avoids
    // bloating the header.
    char sig_hex[33];
    bytes_to_hex(mac, 16, sig_hex);

    // Peer id full hex (32 chars) — listener uses this to look up the
    // matching bond locally.
    char pid_hex[SK_API_PEER_ID_LEN * 2 + 1];
    bytes_to_hex(ep->peer_id, SK_API_PEER_ID_LEN, pid_hex);

    esp_http_client_set_header(c, "X-SK-Device-Id", sk_identity_get());
    esp_http_client_set_header(c, "X-SK-Peer-Id",   pid_hex);
    esp_http_client_set_header(c, "X-SK-Timestamp", ts_str);
    esp_http_client_set_header(c, "X-SK-Nonce",     nonce_hex);
    esp_http_client_set_header(c, "X-SK-Signature", sig_hex);
    return SK_OK;
}

// Emits the auth header(s) for USER kind. Returns SK_OK on success.
static sk_err_t apply_auth(esp_http_client_handle_t c, const sk_api_endpoint_t *ep,
                           char *scratch, size_t scratch_cap)
{
    switch (ep->auth) {
    case SK_API_AUTH_NONE:
        return SK_OK;

    case SK_API_AUTH_BEARER:
        if (!ep->token[0]) return SK_ERR_API_NOT_CONFIGURED;
        snprintf(scratch, scratch_cap, "Bearer %s", ep->token);
        esp_http_client_set_header(c, "Authorization", scratch);
        return SK_OK;

    case SK_API_AUTH_BASIC: {
        if (!ep->token[0]) return SK_ERR_API_NOT_CONFIGURED;
        unsigned char enc[256];
        size_t enc_len = 0;
        int rc = mbedtls_base64_encode(enc, sizeof(enc), &enc_len,
                                       (const unsigned char *)ep->token,
                                       strlen(ep->token));
        if (rc != 0 || enc_len + 1 >= sizeof(enc)) return SK_ERR_API_NOT_CONFIGURED;
        enc[enc_len] = '\0';
        snprintf(scratch, scratch_cap, "Basic %s", (char *)enc);
        esp_http_client_set_header(c, "Authorization", scratch);
        return SK_OK;
    }

    case SK_API_AUTH_CUSTOM_HEADER:
        if (!ep->token[0] || !ep->header_name[0]) return SK_ERR_API_NOT_CONFIGURED;
        esp_http_client_set_header(c, ep->header_name, ep->token);
        return SK_OK;

    default:
        return SK_ERR_API_INVALID_TYPE;
    }
}

// Builds URL + body for the given endpoint type. Method/auth/content_type
// for non-preset types come from the endpoint record itself; preset types
// (IFTTT) override and ignore those fields.
static sk_err_t build_request(const sk_api_endpoint_t *ep,
                              const char *payload,
                              char *out_url, size_t url_cap,
                              char *out_body, size_t body_cap)
{
    switch (ep->type) {
    case SK_API_GENERIC:
    case SK_API_WEBHOOK_POST:
        snprintf(out_url, url_cap, "%s", ep->url);
        snprintf(out_body, body_cap, "%s", payload ? payload : "");
        return SK_OK;

    // SK_API_TELEGRAM removed — value 1 falls into default: case below
    // (SK_ERR_API_INVALID_TYPE), which surfaces legacy NVS records as a
    // clear error rather than silently sending the wrong thing.

    case SK_API_IFTTT:
        if (!ep->token[0]) return SK_ERR_API_NOT_CONFIGURED;
        snprintf(out_url, url_cap,
                 "https://maker.ifttt.com/trigger/%s/with/key/%s",
                 ep->url, ep->token);
        snprintf(out_body, body_cap, "%s",
                 (payload && payload[0]) ? payload : "{}");
        return SK_OK;

    default:
        return SK_ERR_API_INVALID_TYPE;
    }
}

// Resolves effective method + content-type. Preset types override; generic
// types use the endpoint record (with sane defaults).
static void resolve_method_ct(const sk_api_endpoint_t *ep,
                              esp_http_client_method_t *out_method,
                              const char **out_ct)
{
    switch (ep->type) {
    case SK_API_IFTTT:
        // IFTTT preset is always POST + JSON regardless of the saved fields.
        *out_method = HTTP_METHOD_POST;
        *out_ct     = DEFAULT_CT;
        return;
    default:
        *out_method = method_to_esp(ep->method);
        *out_ct     = ep->content_type[0] ? ep->content_type : DEFAULT_CT;
        return;
    }
}

static bool method_has_body(esp_http_client_method_t m)
{
    return m == HTTP_METHOD_POST || m == HTTP_METHOD_PUT || m == HTTP_METHOD_DELETE;
}

// Compact reason classifier for SK_LOG endpoint.fail messages. Keeps the
// vocabulary small so log scrapers can grep: timeout / dns / tls / network /
// http_<status> / config.
static const char *fail_reason(esp_err_t err, int status)
{
    if (status >= 400 && status < 600) {
        static char buf[16];
        snprintf(buf, sizeof(buf), "http_%d", status);
        return buf;
    }
    switch (err) {
    case ESP_ERR_TIMEOUT:                return "timeout";
    case ESP_ERR_HTTP_EAGAIN:            return "timeout";
    case ESP_ERR_HTTP_INVALID_TRANSPORT: return "tls";
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_CONNECTING:
    case ESP_ERR_HTTP_FETCH_HEADER:      return "network";
    default:                             return status == 0 ? "network" : "unknown";
    }
}

static void send_worker(void *arg)
{
    send_job_t *job = (send_job_t *)arg;

    int64_t t_start = esp_timer_get_time();

    char  url[256];
    char  body[1024];
    char  auth_scratch[200];

    sk_err_t berr = build_request(&job->ep, job->payload,
                                  url, sizeof(url),
                                  body, sizeof(body));
    if (berr != SK_OK) {
        worker_publish_result(job->ep.name, false, 0, berr);
        free(job->payload); free(job); vTaskDelete(NULL); return;
    }

    esp_http_client_method_t method;
    const char              *ct;
    resolve_method_ct(&job->ep, &method, &ct);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = method,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    // Faz 2: retry zinciri. status==0 (bağlantı kurulamadı / read timeout)
    // durumunda max 1 retry, 200ms backoff. Status >0 (4xx/5xx) durumunda
    // istek SKAPP'a ulaşmış sayılır, retry YAPILMAZ (idempotency riski).
    // apply_bond_signature her çağrıda yeni nonce+timestamp+HMAC üretir,
    // dedup ring'i kirletmez.
    esp_http_client_handle_t c = NULL;
    esp_err_t err = ESP_OK;
    int status = 0;
    const int max_attempts = 2;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        c = esp_http_client_init(&cfg);
        if (!c) {
            worker_publish_result(job->ep.name, false, 0, SK_ERR_INTERNAL);
            free(job->payload); free(job); vTaskDelete(NULL); return;
        }

        esp_http_client_set_header(c, "Content-Type", ct);

        // SYSTEM kind: bond-HMAC headers replace any conventional auth.
        // USER kind: stored auth scheme. IFTTT preset always overrides to NONE.
        sk_err_t aerr = SK_OK;
        if (job->ep.kind == SK_API_KIND_SYSTEM) {
            aerr = apply_bond_signature(c, &job->ep, body);
        } else {
            sk_api_endpoint_t auth_view = job->ep;
            if (auth_view.type == SK_API_IFTTT) auth_view.auth = SK_API_AUTH_NONE;
            aerr = apply_auth(c, &auth_view, auth_scratch, sizeof(auth_scratch));
        }
        if (aerr != SK_OK) {
            esp_http_client_cleanup(c);
            worker_publish_result(job->ep.name, false, 0, aerr);
            free(job->payload); free(job); vTaskDelete(NULL); return;
        }

        if (method_has_body(method) && body[0]) {
            esp_http_client_set_post_field(c, body, (int)strlen(body));
        }

        // FAZ0_DIAG: HTTP perform delta. Beklenen ayrıştırma:
        //   ~3000ms   -> HTTP_TIMEOUT_MS (Faz 1 sonrası yeni limit)
        //   1-3000ms  -> DNS/TLS overhead (Faz 3 tetikleyici)
        //   <500ms    -> firmware HTTP sorun değil, gecikme SKAPP tarafında
        int64_t t_perform = esp_timer_get_time();
        err = esp_http_client_perform(c);
        status = esp_http_client_get_status_code(c);
        ESP_LOGW(TAG, "FAZ0_DIAG http_perform attempt=%d/%d delta_ms=%lld status=%d err=%s",
                 attempt + 1, max_attempts,
                 (esp_timer_get_time() - t_perform) / 1000,
                 status,
                 esp_err_to_name(err));
        esp_http_client_cleanup(c);
        c = NULL;

        // Retry sadece "istek ulaşmadı" senaryosunda: err != ESP_OK ve
        // status == 0. status > 0 → server cevap verdi (idempotency riski).
        bool retriable = (err != ESP_OK) && (status == 0);
        if (!retriable) break;
        if (attempt + 1 < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;

    if (err != ESP_OK) {
        worker_publish_result(job->ep.name, false, 0, map_http_err(err));
        SK_LOG_E("api", "endpoint.fail", "name=%s reason=%s",
                 job->ep.name, fail_reason(err, 0));
    } else if (status >= 200 && status < 300) {
        worker_publish_result(job->ep.name, true, status, SK_OK);
        SK_LOG_I("api", "endpoint.fire", "name=%s status=%d ms=%lld",
                 job->ep.name, status, (long long)elapsed_ms);
    } else {
        worker_publish_result(job->ep.name, false, status, SK_ERR_API_BAD_STATUS);
        SK_LOG_E("api", "endpoint.fail", "name=%s reason=%s",
                 job->ep.name, fail_reason(ESP_OK, status));
    }

    free(job->payload);
    free(job);
    vTaskDelete(NULL);
}

esp_err_t sk_api_send(const char *name, const char *payload)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    if (!sk_api_is_enabled_all()) return ESP_ERR_INVALID_STATE;
    sk_api_endpoint_t *ep = find_any_by_name(name);
    if (!ep) return ESP_ERR_NOT_FOUND;

    sk_wifi_status_t wstat;
    sk_wifi_status(&wstat);
    if (!wstat.connected) {
        worker_publish_result(name, false, 0, SK_ERR_API_OFFLINE);
        return ESP_ERR_INVALID_STATE;
    }

    send_job_t *job = malloc(sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    job->ep      = *ep;
    job->payload = payload ? strdup(payload) : strdup("");
    if (!job->payload) { free(job); return ESP_ERR_NO_MEM; }

    char taskname[8 + SK_API_NAME_MAX + 1];
    snprintf(taskname, sizeof(taskname), "sk_api_%.*s", SK_API_NAME_MAX, name);
    BaseType_t ok = xTaskCreate(send_worker, taskname, WORKER_STACK,
                                job, 4, NULL);
    if (ok != pdPASS) {
        free(job->payload); free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// -- Chain runner ----------------------------------------------------------
//
// Chain order: USER slots first (index 0→USER_SLOTS-1), then SYSTEM slots
// (index 0→SYSTEM_SLOTS-1). Each slot's `delay_after_sec` is honoured
// between fires; offline slots fail individually without blocking the
// chain (sk_api_send is async, only the inter-slot delay loop blocks).

typedef struct {
    char     names[SK_API_MAX_ENDPOINTS][SK_API_NAME_MAX + 1];
    uint16_t delays[SK_API_MAX_ENDPOINTS];
    int      count;
    sk_api_trigclass_t cls;   // class that ran (only meaningful when filtered)
    bool     class_filtered;  // true → api.chain.finished includes "class"
    char     payload[SK_API_PAYLOAD_MAX + 1];
} chain_job_t;

static void chain_runner(void *arg)
{
    chain_job_t *job = (chain_job_t *)arg;
    int ok_count = 0;

    for (int i = 0; i < job->count; i++) {
        // Endpoint may have been removed between snapshot and fire —
        // skip silently in that case.
        if (!find_any_by_name(job->names[i])) continue;

        esp_err_t err = sk_api_send(job->names[i], job->payload);
        if (err == ESP_OK) ok_count++;

        // Last-step delay is meaningless; vTaskDelay(0) is a no-op so
        // we only call it for positive intervals.
        if (i < job->count - 1 && job->delays[i] > 0) {
            vTaskDelay(pdMS_TO_TICKS((TickType_t)job->delays[i] * 1000));
        }
    }

    char buf[96];
    if (job->class_filtered) {
        snprintf(buf, sizeof(buf),
                 "{\"count\":%d,\"ok_count\":%d,\"class\":\"%s\"}",
                 job->count, ok_count, sk_api_trigclass_str(job->cls));
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"count\":%d,\"ok_count\":%d}", job->count, ok_count);
    }
    sk_event_bus_publish("api.chain.finished", buf);

    free(job);
    vTaskDelete(NULL);
}

// Build the chain snapshot (USER first, then SYSTEM) and spawn the runner.
// When `class_filtered`, only slots whose trigclass matches `cls` are
// included (a slot tagged BOTH matches any class).
static esp_err_t chain_build_and_run(bool class_filtered,
                                     sk_api_trigclass_t cls,
                                     const char *payload)
{
    if (!sk_api_is_enabled_all()) return ESP_ERR_INVALID_STATE;

    chain_job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;

    int n = 0;
    // USER first
    for (int i = 0; i < SK_API_USER_SLOTS; i++) {
        if (!s_user[i].in_use) continue;
        if (class_filtered && !trigclass_matches(s_user[i].trigclass, cls)) continue;
        strncpy(job->names[n], s_user[i].name, SK_API_NAME_MAX);
        job->delays[n] = s_user[i].delay_after_sec;
        n++;
    }
    // SYSTEM after
    for (int i = 0; i < SK_API_SYSTEM_SLOTS; i++) {
        if (!s_system[i].in_use) continue;
        if (class_filtered && !trigclass_matches(s_system[i].trigclass, cls)) continue;
        strncpy(job->names[n], s_system[i].name, SK_API_NAME_MAX);
        job->delays[n] = s_system[i].delay_after_sec;
        n++;
    }
    if (n == 0) {
        free(job);
        return ESP_ERR_NOT_FOUND;
    }
    job->count          = n;
    job->cls            = cls;
    job->class_filtered = class_filtered;
    if (payload) {
        strncpy(job->payload, payload, SK_API_PAYLOAD_MAX);
        job->payload[SK_API_PAYLOAD_MAX] = '\0';
    }

    BaseType_t ok =
        xTaskCreate(chain_runner, "sk_api_chain", 4096, job, 4, NULL);
    if (ok != pdPASS) {
        free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sk_api_chain_run(const char *payload)
{
    return chain_build_and_run(false, SK_API_TRIGCLASS_TRIGGER, payload);
}

esp_err_t sk_api_chain_run_class(sk_api_trigclass_t cls, const char *payload)
{
    return chain_build_and_run(true, cls, payload);
}

// -- CLI handlers ----------------------------------------------------------

static sk_err_t cmd_api_enable(sk_cli_ctx_t *ctx)
{
    sk_api_set_enabled_all(true);
    sk_cli_ok(ctx, "{\"enabled\":true}");
    return SK_OK;
}

static sk_err_t cmd_api_disable(sk_cli_ctx_t *ctx)
{
    sk_api_set_enabled_all(false);
    sk_cli_ok(ctx, "{\"enabled\":false}");
    return SK_OK;
}

static sk_err_t cmd_api_status(sk_cli_ctx_t *ctx)
{
    int user_count = 0, sys_count = 0;
    for (int i = 0; i < SK_API_USER_SLOTS;   i++) if (s_user[i].in_use)   user_count++;
    for (int i = 0; i < SK_API_SYSTEM_SLOTS; i++) if (s_system[i].in_use) sys_count++;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"master_enabled\":%s,\"endpoints\":%d,"
             "\"user_endpoints\":%d,\"system_endpoints\":%d,"
             "\"user_slots\":%d,\"system_slots\":%d}",
             sk_api_is_enabled_all() ? "true" : "false",
             user_count + sys_count,
             user_count, sys_count,
             SK_API_USER_SLOTS, SK_API_SYSTEM_SLOTS);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static void mask_token(const char *tok, char *out, size_t cap)
{
    size_t n = strlen(tok);
    if (n == 0)  { out[0] = '\0'; return; }
    if (n < 12)  { snprintf(out, cap, "****"); return; }
    snprintf(out, cap, "%.4s...%s", tok, tok + n - 4);
}

// Append one endpoint as JSON to the buffer at *off. Returns updated
// offset; truncation is silent (off may exceed BUF, in which case the
// caller still emits the closing bracket and the result is invalid JSON
// — but with BUF=2048 vs the bounded slot count this is practical).
static size_t append_ep_json(char *buf, size_t off, size_t cap,
                             const sk_api_endpoint_t *ep, int slot,
                             bool first)
{
    char masked[SK_API_TOKEN_MAX + 1] = {0};
    mask_token(ep->token, masked, sizeof(masked));
    char pid_hex[SK_API_PEER_ID_LEN * 2 + 1] = {0};
    if (ep->kind == SK_API_KIND_SYSTEM) {
        bytes_to_hex(ep->peer_id, SK_API_PEER_ID_LEN, pid_hex);
    }
    return off + snprintf(buf + off, cap - off,
                          "%s{\"slot\":%d,\"kind\":\"%s\",\"class\":\"%s\","
                          "\"name\":\"%s\","
                          "\"type\":\"%s\",\"url\":\"%s\","
                          "\"method\":\"%s\",\"auth\":\"%s\",\"header\":\"%s\","
                          "\"content_type\":\"%s\",\"masked_token\":\"%s\","
                          "\"delay_after_sec\":%u,\"peer_id\":\"%s\"}",
                          first ? "" : ",",
                          slot,
                          sk_api_kind_str(ep->kind),
                          sk_api_trigclass_str(ep->trigclass),
                          ep->name,
                          sk_api_type_str(ep->type),
                          ep->url,
                          sk_api_method_str(ep->method),
                          sk_api_auth_str(ep->auth),
                          ep->header_name,
                          ep->content_type,
                          masked,
                          (unsigned)ep->delay_after_sec,
                          pid_hex);
}

static sk_err_t cmd_api_endpoint_list(sk_cli_ctx_t *ctx)
{
    enum { BUF = 4096 };
    char *buf = malloc(BUF);
    if (!buf) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"oom\"}");
        return SK_OK;
    }

    size_t off = 0;
    off += snprintf(buf + off, BUF - off, "[");
    bool first = true;
    for (int i = 0; i < SK_API_USER_SLOTS; i++) {
        if (!s_user[i].in_use) continue;
        off = append_ep_json(buf, off, BUF, &s_user[i], i, first);
        first = false;
    }
    for (int i = 0; i < SK_API_SYSTEM_SLOTS; i++) {
        if (!s_system[i].in_use) continue;
        off = append_ep_json(buf, off, BUF, &s_system[i], i, first);
        first = false;
    }
    if (off < BUF) off += snprintf(buf + off, BUF - off, "]");
    sk_cli_ok(ctx, buf);

    free(buf);
    return SK_OK;
}

static sk_err_t cmd_api_endpoint_add(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }

    const char *name   = sk_cli_arg_named(ctx, "name");
    const char *typs   = sk_cli_arg_named(ctx, "type");
    const char *url    = sk_cli_arg_named(ctx, "url");
    const char *token  = sk_cli_arg_named(ctx, "token");
    const char *meths  = sk_cli_arg_named(ctx, "method");
    const char *auths  = sk_cli_arg_named(ctx, "auth");
    const char *hdr    = sk_cli_arg_named(ctx, "header-name");
    const char *ct     = sk_cli_arg_named(ctx, "content-type");
    const char *delays = sk_cli_arg_named(ctx, "delay-after");
    const char *clss   = sk_cli_arg_named(ctx, "class");

    if (!name || !typs || !url) {
        sk_cli_err(ctx, SK_ERR_MISSING_ARG, "{\"need\":[\"name\",\"type\",\"url\"]}");
        return SK_OK;
    }

    int t = sk_api_type_from_str(typs);
    if (t < 0) { sk_cli_err(ctx, SK_ERR_API_INVALID_TYPE, NULL); return SK_OK; }

    sk_api_method_t method = SK_API_METHOD_POST;
    if (meths) {
        int m = sk_api_method_from_str(meths);
        if (m < 0) { sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"method\"}"); return SK_OK; }
        method = (sk_api_method_t)m;
    }

    sk_api_auth_t auth = SK_API_AUTH_NONE;
    if (auths) {
        int a = sk_api_auth_from_str(auths);
        if (a < 0) { sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"auth\"}"); return SK_OK; }
        auth = (sk_api_auth_t)a;
    }

    uint16_t delay_after = 0;
    if (delays) {
        long d = strtol(delays, NULL, 10);
        if (d < 0 || d > SK_API_DELAY_AFTER_MAX_SEC) {
            sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"delay-after\"}");
            return SK_OK;
        }
        delay_after = (uint16_t)d;
    }

    sk_api_trigclass_t trigclass = SK_API_TRIGCLASS_TRIGGER;
    if (clss) {
        int c = sk_api_trigclass_from_str(clss);
        if (c < 0) { sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"class\"}"); return SK_OK; }
        trigclass = (sk_api_trigclass_t)c;
    }

    if (t == SK_API_IFTTT && (!token || !token[0])) {
        sk_cli_err(ctx, SK_ERR_MISSING_ARG,
                   "{\"field\":\"token\",\"hint\":\"ifttt maker key\"}");
        return SK_OK;
    }
    if (auth == SK_API_AUTH_CUSTOM_HEADER && (!hdr || !hdr[0])) {
        sk_cli_err(ctx, SK_ERR_MISSING_ARG, "{\"field\":\"header-name\"}");
        return SK_OK;
    }

    esp_err_t err = sk_api_endpoint_add(&(sk_api_endpoint_cfg_t){
        .name             = name,
        .type             = (sk_api_type_t)t,
        .url              = url,
        .token            = token,
        .method           = method,
        .auth             = auth,
        .header_name      = hdr,
        .content_type     = ct,
        .delay_after_sec  = delay_after,
        .trigclass        = trigclass,
    });
    if (err == ESP_ERR_NO_MEM) { sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"slots_full\"}"); return SK_OK; }
    if (err != ESP_OK)         { sk_cli_err(ctx, SK_ERR_INVALID_ARG, NULL); return SK_OK; }
    sk_cli_ok(ctx, "{\"added\":true}");
    return SK_OK;
}

static sk_err_t cmd_api_endpoint_remove(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    const char *name = sk_cli_arg_named(ctx, "name");
    if (!name) { sk_cli_err(ctx, SK_ERR_MISSING_ARG, "{\"field\":\"name\"}"); return SK_OK; }
    if (sk_api_endpoint_remove(name) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_OK;
    }
    sk_cli_ok(ctx, "{\"removed\":true}");
    return SK_OK;
}

// -- SYSTEM slot CLI handlers ---------------------------------------------
//
// These run on the authenticated CLI surface only. The active session's
// bond slot determines which peer_id the SYSTEM record is keyed by — the
// peer cannot register an endpoint on behalf of someone else (cross-tenant
// guard). If args also include `peer_id` it must match the active one.

static esp_err_t resolve_caller_peer_id(sk_cli_ctx_t *ctx,
                                        uint8_t out[SK_API_PEER_ID_LEN])
{
    if (!sk_cli_is_authenticated(ctx)) return ESP_ERR_INVALID_STATE;
    uint8_t slot = sk_auth_active_bond_slot();
    if (slot == SK_AUTH_BOND_SLOT_INVALID) return ESP_ERR_INVALID_STATE;

    sk_auth_bond_info_t bonds[SK_AUTH_BOND_SLOT_COUNT];
    uint8_t count = 0;
    if (sk_auth_bond_list(bonds, &count) != ESP_OK) return ESP_FAIL;
    for (uint8_t i = 0; i < count; i++) {
        if (bonds[i].slot == slot) {
            memcpy(out, bonds[i].peer_id, SK_API_PEER_ID_LEN);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static sk_err_t cmd_api_system_add(sk_cli_ctx_t *ctx)
{
    if (!sk_cli_is_authenticated(ctx)) {
        sk_cli_err(ctx, SK_ERR_NOT_AUTHENTICATED, NULL);
        return SK_OK;
    }
    const char *url    = sk_cli_arg_named(ctx, "url");
    const char *delays = sk_cli_arg_named(ctx, "delay-after");
    if (!url) { sk_cli_err(ctx, SK_ERR_MISSING_ARG, "{\"field\":\"url\"}"); return SK_OK; }

    uint16_t delay_after = 0;
    if (delays) {
        long d = strtol(delays, NULL, 10);
        if (d < 0 || d > SK_API_DELAY_AFTER_MAX_SEC) {
            sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"delay-after\"}");
            return SK_OK;
        }
        delay_after = (uint16_t)d;
    }

    uint8_t peer_id[SK_API_PEER_ID_LEN];
    if (resolve_caller_peer_id(ctx, peer_id) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_NOT_AUTHENTICATED, NULL);
        return SK_OK;
    }

    uint8_t out_slot = 0;
    esp_err_t err = sk_api_system_add(&(sk_api_system_cfg_t){
        .peer_id         = peer_id,
        .url             = url,
        .delay_after_sec = delay_after,
    }, &out_slot);
    if (err == ESP_ERR_NO_MEM)  { sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"slots_full\"}"); return SK_OK; }
    if (err == ESP_ERR_NOT_FOUND) { sk_cli_err(ctx, SK_ERR_NOT_FOUND, "{\"reason\":\"unknown peer_id\"}"); return SK_OK; }
    if (err != ESP_OK)          { sk_cli_err(ctx, SK_ERR_INVALID_ARG, NULL); return SK_OK; }

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"slot\":%u}", (unsigned)out_slot);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static sk_err_t cmd_api_system_remove(sk_cli_ctx_t *ctx)
{
    if (!sk_cli_is_authenticated(ctx)) {
        sk_cli_err(ctx, SK_ERR_NOT_AUTHENTICATED, NULL);
        return SK_OK;
    }
    uint8_t peer_id[SK_API_PEER_ID_LEN];
    if (resolve_caller_peer_id(ctx, peer_id) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_NOT_AUTHENTICATED, NULL);
        return SK_OK;
    }
    if (sk_api_system_remove(peer_id) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_OK;
    }
    sk_cli_ok(ctx, "{\"removed\":true}");
    return SK_OK;
}

// Wipe every SYSTEM slot. Authenticated, but ownership-blind: the caller
// is asking the device to forget every paired-SKAPP listener URL. Used
// when SKAPP detects orphan slots from a previous install (lost peer_id)
// — the active SKAPP re-publishes its own slot via api.system.add right
// after this returns. Multi-laptop callers should be aware that this
// also wipes the OTHER laptops' slots (they'll re-publish on next sync).
static sk_err_t cmd_api_system_purge(sk_cli_ctx_t *ctx)
{
    if (!sk_cli_is_authenticated(ctx)) {
        sk_cli_err(ctx, SK_ERR_NOT_AUTHENTICATED, NULL);
        return SK_OK;
    }
    if (sk_api_system_remove_all() != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, NULL);
        return SK_OK;
    }
    sk_cli_ok(ctx, "{\"purged\":true}");
    return SK_OK;
}

static sk_err_t cmd_api_system_list(sk_cli_ctx_t *ctx)
{
    enum { BUF = 1024 };
    char *buf = malloc(BUF);
    if (!buf) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"oom\"}");
        return SK_OK;
    }
    size_t off = 0;
    off += snprintf(buf + off, BUF - off, "[");
    bool first = true;
    for (int i = 0; i < SK_API_SYSTEM_SLOTS; i++) {
        if (!s_system[i].in_use) continue;
        off = append_ep_json(buf, off, BUF, &s_system[i], i, first);
        first = false;
    }
    if (off < BUF) off += snprintf(buf + off, BUF - off, "]");
    sk_cli_ok(ctx, buf);
    free(buf);
    return SK_OK;
}

static sk_err_t cmd_api_send(sk_cli_ctx_t *ctx)
{
    const char *name    = sk_cli_arg_named(ctx, "name");
    const char *payload = sk_cli_arg_named(ctx, "payload");
    if (!name) { sk_cli_err(ctx, SK_ERR_MISSING_ARG, "{\"field\":\"name\"}"); return SK_OK; }
    if (!sk_api_is_enabled_all()) {
        sk_cli_err(ctx, SK_ERR_API_DISABLED, NULL);
        return SK_OK;
    }
    char default_payload[160];
    if (!payload || !*payload) {
        snprintf(default_payload, sizeof(default_payload),
                 "{\"event\":\"manual_test\","
                 "\"device\":\"%s\","
                 "\"value1\":\"manual_test\"}",
                 sk_identity_get());
        payload = default_payload;
    }
    esp_err_t err = sk_api_send(name, payload);
    if (err == ESP_ERR_NOT_FOUND)     { sk_cli_err(ctx, SK_ERR_API_NOT_FOUND, NULL); return SK_OK; }
    if (err == ESP_ERR_INVALID_STATE) { sk_cli_err(ctx, SK_ERR_API_OFFLINE, NULL);   return SK_OK; }
    if (err != ESP_OK)                { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL);     return SK_OK; }
    sk_cli_ok(ctx, "{\"queued\":true}");
    return SK_OK;
}

static sk_err_t cmd_api_chain_run(sk_cli_ctx_t *ctx)
{
    const char *payload = sk_cli_arg_named(ctx, "payload");
    if (!sk_api_is_enabled_all()) {
        sk_cli_err(ctx, SK_ERR_API_DISABLED, NULL);
        return SK_OK;
    }
    esp_err_t err = sk_api_chain_run(payload);
    if (err == ESP_ERR_NOT_FOUND) {
        sk_cli_err(ctx, SK_ERR_API_NOT_FOUND, NULL);
        return SK_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        sk_cli_err(ctx, SK_ERR_API_DISABLED, NULL);
        return SK_OK;
    }
    if (err != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, NULL);
        return SK_OK;
    }
    sk_cli_ok(ctx, "{\"chain\":\"running\"}");
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .requires_auth = true, .name = "api.on",  .summary = "Master switch — allow stored endpoints to fire",
      .usage = "api on",
      .help_block =
          "When a countdown reaches zero, every stored endpoint fires only\n"
          "if the master switch is on. Endpoint records (api.endpoint.add)\n"
          "are kept regardless; this gate just decides whether they fire.\n"
          "\n"
          "Example:\n"
          "  api on",
      .handler = cmd_api_enable },

    { .requires_auth = true, .name = "api.off", .summary = "Master switch — block all API calls (records kept)",
      .usage = "api off",
      .help_block =
          "Blocks every outbound endpoint from firing without deleting your\n"
          "endpoint records. Useful as a quick \"silent mode\" — don't trigger\n"
          "IFTTT / Home Assistant for this session, but keep the setup.\n"
          "\n"
          "Example:\n"
          "  api off",
      .handler = cmd_api_disable },

    { .requires_auth = true, .name = "api.status", .summary = "Show master switch state and endpoint counts",
      .usage = "api status",
      .help_block =
          "Returns the master switch state and how many endpoints are\n"
          "stored, broken down by kind (USER manual webhooks vs SYSTEM\n"
          "auto-managed paired-SKAPP listeners).\n"
          "\n"
          "Example output:\n"
          "  { \"master_enabled\": true, \"endpoints\": 3,\n"
          "    \"user_endpoints\": 2, \"system_endpoints\": 1,\n"
          "    \"user_slots\": 5, \"system_slots\": 8 }",
      .handler = cmd_api_status },

    { .requires_auth = true, .name = "api.endpoint.list", .summary = "List stored endpoints (USER + SYSTEM)",
      .usage = "api endpoint list",
      .help_block =
          "Returns every stored endpoint with its full configuration. Token\n"
          "values are redacted. USER slots come first (chain order), then\n"
          "SYSTEM slots (auto-managed by paired SKAPPs).\n"
          "\n"
          "Example:\n"
          "  api endpoint list",
      .handler = cmd_api_endpoint_list },

    { .requires_auth = true, .name = "api.endpoint.add", .summary = "Add or update a USER (manual) endpoint",
      .usage = "api endpoint add --name X --type generic|ifttt|webhook_post --url ... "
               "[--token ...] [--method POST|GET|PUT|DELETE] "
               "[--auth none|bearer|basic|header] "
               "[--header-name X-API-Key] [--content-type application/json] "
               "[--delay-after <seconds 0-300>] [--class trigger|alarm|both]",
      .critical = true,
      .help_block =
          "Creates a new USER (manual) endpoint or overwrites the existing\n"
          "one with the same --name. Persisted in NVS. Survives reboot.\n"
          "USER slots are limited to SK_API_USER_SLOTS (5).\n"
          "\n"
          "SYSTEM (auto-managed) endpoints are NOT added through this\n"
          "command — paired SKAPPs use api.system.add instead.\n"
          "\n"
          "Types:\n"
          "  ifttt        IFTTT Maker webhook preset\n"
          "  webhook_post Generic POST with --auth control\n"
          "  generic      Any method / auth / header combination\n"
          "\n"
          "--class controls which event layer auto-fires this endpoint:\n"
          "  trigger  timer.triggered (deadline reached) — DEFAULT\n"
          "  alarm    timer.alarm (early warning, fires N times before)\n"
          "  both     fires on either layer\n"
          "Use alarm for reminder/Telegram webhooks, trigger for the final\n"
          "consequence webhook. (Telegram delivery = a generic/webhook_post\n"
          "endpoint pointed at a bot proxy URL, with --class alarm.)\n"
          "\n"
          "--delay-after seconds (0-300) is wait time AFTER this endpoint\n"
          "fires, before the next one in api.chain.run.\n"
          "\n"
          "Examples:\n"
          "  # IFTTT Maker applet\n"
          "  api endpoint add --name lights --type ifttt \\\n"
          "    --url focus_done --token <KEY> --confirm-token <T>\n"
          "\n"
          "  # Generic POST with bearer auth + 5s post-fire delay\n"
          "  api endpoint add --name ha --type webhook_post \\\n"
          "    --url https://homeassistant.local/api/services/script/focus \\\n"
          "    --auth bearer --token <ACCESS_TOKEN> \\\n"
          "    --content-type application/json --delay-after 5 \\\n"
          "    --confirm-token <T>",
      .handler = cmd_api_endpoint_add },

    { .requires_auth = true, .name = "api.endpoint.remove", .summary = "Delete a USER endpoint by name",
      .usage = "api endpoint remove --name X",
      .critical = true,
      .help_block =
          "Removes the USER endpoint record from NVS. SYSTEM slots are\n"
          "removed via api.system.remove (or by the paired SKAPP itself).\n"
          "\n"
          "Example:\n"
          "  api endpoint remove --name lights --confirm-token <T>",
      .handler = cmd_api_endpoint_remove },

    { .requires_auth = true, .name = "api.system.add",
      .summary = "Register the calling peer's SKAPP listener as a SYSTEM slot",
      .usage = "api system add --url http://<host>:<port>/api/events/incoming "
               "[--delay-after <seconds 0-300>]",
      .help_block =
          "Auto-managed: the active CLI session must be authenticated; the\n"
          "peer_id is taken from the session bond. Each peer_id may register\n"
          "exactly one URL — re-running the command upserts the URL/delay.\n"
          "\n"
          "The fired requests carry X-SK-* headers (Device-Id, Peer-Id,\n"
          "Timestamp, Nonce, Signature). The signature is HMAC-SHA256\n"
          "over the body using the bond key, truncated to 16 bytes.\n"
          "\n"
          "Use this from SKAPP when the user binds a script in SKAPI; do\n"
          "not call it manually unless you know what you're doing.\n"
          "\n"
          "Example:\n"
          "  api system add --url http://192.168.1.10:5000/api/events/incoming",
      .handler = cmd_api_system_add },

    { .requires_auth = true, .name = "api.system.remove",
      .summary = "Unregister the calling peer's SYSTEM slot",
      .usage = "api system remove",
      .help_block =
          "Removes the SYSTEM endpoint owned by the active session's peer.\n"
          "Used when SKAPP wants to stop receiving webhooks (e.g. user\n"
          "deleted their last script binding) without unpairing.",
      .handler = cmd_api_system_remove },

    { .requires_auth = true, .name = "api.system.purge",
      .summary = "Wipe ALL SYSTEM slots (orphan cleanup)",
      .usage = "api system purge",
      .help_block =
          "Empties every SYSTEM slot regardless of which peer_id owns it.\n"
          "Authenticated but ownership-blind. The device will not fire\n"
          "any SYSTEM webhook after this until a fresh api.system.add.\n"
          "\n"
          "Reason: when a SKAPP install is uninstalled without unpairing\n"
          "(or its secure-storage was wiped), its peer_id is lost and the\n"
          "per-peer api.system.remove can no longer reach those slots.\n"
          "BF would keep firing the orphan URL on every countdown and\n"
          "block on the 10-second TCP timeout. Calling api.system.purge\n"
          "resets the bucket; the active SKAPP should immediately follow\n"
          "with its own api.system.add to re-establish the listener URL.",
      .handler = cmd_api_system_purge },

    { .requires_auth = true, .name = "api.system.list",
      .summary = "List SYSTEM slots only (auto-managed by paired SKAPPs)",
      .usage = "api system list",
      .help_block =
          "Diagnostic listing of just the SYSTEM bucket. Same row shape as\n"
          "api.endpoint.list but filtered to kind=system.",
      .handler = cmd_api_system_list },

    { .requires_auth = true, .name = "api.send", .summary = "Fire one endpoint right now (manual test)",
      .usage = "api send --name X [--payload '...']",
      .help_block =
          "Triggers a single endpoint immediately, bypassing the timer.\n"
          "Async — the command returns ok before the HTTP call completes;\n"
          "the result is emitted as an api.sent event.\n"
          "\n"
          "Looks up across BOTH USER and SYSTEM buckets by name.\n"
          "\n"
          "Examples:\n"
          "  api send --name notify\n"
          "  api send --name notify --payload '{\"text\":\"manual test\"}'",
      .handler = cmd_api_send },

    { .requires_auth = true, .name = "api.chain.run", .summary = "Fire every endpoint (USER then SYSTEM, in order)",
      .usage = "api chain run [--payload '...']",
      .help_block =
          "Fires every stored endpoint sequentially, IGNORING trigger class\n"
          "(both alarm- and trigger-class slots fire). USER slots first\n"
          "(index 0..N), then SYSTEM slots (index 0..M). Each slot's\n"
          "delay-after is honoured between fires.\n"
          "\n"
          "Note: the autonomous timer path is class-filtered — timer.alarm\n"
          "fires only alarm-class slots, timer.triggered only trigger-class.\n"
          "This manual command is the unfiltered fire-all for testing.\n"
          "\n"
          "Examples:\n"
          "  api chain run\n"
          "  api chain run --payload '{\"text\":\"manual chain test\"}'",
      .handler = cmd_api_chain_run },
};

// -- Timer event auto-fire -------------------------------------------------
//
// The device fires its outbound webhook chains autonomously, with no app
// involvement (works while the SKAPP app is closed):
//   timer.alarm     → ALARM-class slots (early-warning reminders)
//   timer.triggered → TRIGGER-class slots (final consequence)
// chain_run_class spawns its own runner task, so kicking it off from the
// synchronous event handler is safe (no blocking on the publisher task).

static void fire_class_for_event(const sk_event_t *evt, sk_api_trigclass_t cls)
{
    const char *pj = (evt && evt->payload_json) ? evt->payload_json : "{}";
    // Runs synchronously on the publisher (timer) task — keep the stack
    // footprint small. Timer payloads are tiny ({"duration_sec":N} etc.),
    // so 256 bytes is ample; chain_run copies it into its own job buffer.
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"%s\",\"device\":\"%s\",\"data\":%s}",
             (evt && evt->name) ? evt->name : "?",
             sk_identity_get(), pj);
    esp_err_t err = sk_api_chain_run_class(cls, payload);
    // NOT_FOUND (no slot for this class) and INVALID_STATE (master switch
    // off) are normal, not errors — the user simply hasn't configured a
    // webhook for this layer. Log only genuine failures.
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
        err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "chain_run_class(%s) failed: %s",
                 sk_api_trigclass_str(cls), esp_err_to_name(err));
    }
}

static void on_timer_alarm(const sk_event_t *evt, void *user)
{
    (void)user;
    fire_class_for_event(evt, SK_API_TRIGCLASS_ALARM);
}

static void on_timer_triggered(const sk_event_t *evt, void *user)
{
    (void)user;
    fire_class_for_event(evt, SK_API_TRIGCLASS_TRIGGER);
}

// -- Factory reset hook ---------------------------------------------------

static void on_factory_reset(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    }
    if (nvs_open(NVS_NS_GLOBAL, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    }
    memset(s_user,   0, sizeof(s_user));
    memset(s_system, 0, sizeof(s_system));
}

// -- Init -----------------------------------------------------------------

esp_err_t sk_api_init(void)
{
    if (s_initialized) return ESP_OK;
    memset(s_user,   0, sizeof(s_user));
    memset(s_system, 0, sizeof(s_system));
    load_all_slots();

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        sk_cli_register(&s_cmds[i]);
    }
    int sub;
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, &sub);
    // Autonomous outbound webhook chains (device-owned, fire app-closed).
    sk_event_bus_subscribe("timer.alarm",     on_timer_alarm,     NULL, &sub);
    sk_event_bus_subscribe("timer.triggered", on_timer_triggered, NULL, &sub);
    sk_capabilities_register_book("sk_api", "0.4.0");
    s_initialized = true;
    ESP_LOGI(TAG,
             "outbound HTTP ready (USER slots %d, SYSTEM slots %d)",
             SK_API_USER_SLOTS, SK_API_SYSTEM_SLOTS);
    return ESP_OK;
}
