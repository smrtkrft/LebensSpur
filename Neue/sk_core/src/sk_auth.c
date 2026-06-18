#include "sk_auth.h"
#include "sk_passphrase.h"
#include "sk_cli.h"
#include "sk_event_bus.h"
#include "sk_capabilities.h"
#include "sk_log.h"

#include "cJSON.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "sk_auth";

#define NVS_NS              "sk_auth"
#define NVS_KEY_LEGACY_TOK  "token"            // pre-multi-bond single-slot key
#define NVS_KEY_BOND_FMT    "bond.%u"          // 0..SK_AUTH_BOND_SLOT_COUNT-1

// On-disk slot layout: peer_id || bond_key || label || paired_at_unix.
// Total = 16 + 32 + 25 + 8 = 81 bytes (label is null-terminated within 25B).
#define SLOT_RECORD_SIZE    (SK_AUTH_PEER_ID_LEN + SK_AUTH_TOKEN_LEN + \
                             (SK_AUTH_BOND_LABEL_MAX + 1) + 8)

typedef struct {
    bool     occupied;
    uint8_t  peer_id[SK_AUTH_PEER_ID_LEN];
    uint8_t  bond_key[SK_AUTH_TOKEN_LEN];
    char     label[SK_AUTH_BOND_LABEL_MAX + 1];
    int64_t  paired_at_unix;
} bond_slot_t;

static bond_slot_t              s_slots[SK_AUTH_BOND_SLOT_COUNT];
static uint8_t                  s_active_slot  = SK_AUTH_BOND_SLOT_INVALID;
static uint8_t                  s_active_key[SK_AUTH_TOKEN_LEN];
static bool                     s_active_set   = false;
static sk_auth_pairing_state_t  s_pairing      = SK_AUTH_PAIRING_IDLE;
static esp_timer_handle_t       s_pair_timer   = NULL;
static SemaphoreHandle_t        s_mtx          = NULL;

// Pending bond — used during the pairing-time passphrase flow. ECDH has
// completed and we have a derived bond key, but we don't commit it to NVS
// until the user proves the passphrase. RAM-only; cleared on commit, on
// pairing-window close, or on too many failed verifies (lockout already
// rolls into a factory-reset via sk_passphrase).
typedef struct {
    bool     active;
    uint8_t  bond_key[SK_AUTH_TOKEN_LEN];
    uint8_t  peer_id[SK_AUTH_PEER_ID_LEN];
    char     label[SK_AUTH_BOND_LABEL_MAX + 1];
    int64_t  expires_us;
} pending_bond_t;

static pending_bond_t s_pending = {0};

static void pending_clear(void)
{
    memset(s_pending.bond_key, 0, sizeof(s_pending.bond_key));
    memset(&s_pending, 0, sizeof(s_pending));
}

// -- Internal helpers ------------------------------------------------------

// Backward-compat shim. The HMAC + handshake layers ask for "the" token via
// these symbols. Semantics changed under the hood:
//   - After a successful handshake_verify_peer, the matched slot's key is
//     installed as the active bond and these return that key.
//   - Before any handshake (or on a fresh begin), they fall back to slot 0
//     so legacy code paths that consult has_token at session-start still
//     work — handshake_begin only needs to know that *some* bond exists.
const uint8_t *sk_auth__token_ptr(void)
{
    if (s_active_set) return s_active_key;
    if (s_slots[0].occupied) return s_slots[0].bond_key;
    return NULL;
}

bool sk_auth__has_token(void)
{
    if (s_active_set) return true;
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        if (s_slots[i].occupied) return true;
    }
    return false;
}

static void slot_record_pack(const bond_slot_t *s, uint8_t out[SLOT_RECORD_SIZE])
{
    size_t off = 0;
    memcpy(out + off, s->peer_id,  SK_AUTH_PEER_ID_LEN); off += SK_AUTH_PEER_ID_LEN;
    memcpy(out + off, s->bond_key, SK_AUTH_TOKEN_LEN);   off += SK_AUTH_TOKEN_LEN;
    memcpy(out + off, s->label,    SK_AUTH_BOND_LABEL_MAX + 1);
    off += (SK_AUTH_BOND_LABEL_MAX + 1);
    memcpy(out + off, &s->paired_at_unix, 8);
}

static void slot_record_unpack(const uint8_t in[SLOT_RECORD_SIZE], bond_slot_t *s)
{
    size_t off = 0;
    memcpy(s->peer_id,  in + off, SK_AUTH_PEER_ID_LEN); off += SK_AUTH_PEER_ID_LEN;
    memcpy(s->bond_key, in + off, SK_AUTH_TOKEN_LEN);   off += SK_AUTH_TOKEN_LEN;
    memcpy(s->label,    in + off, SK_AUTH_BOND_LABEL_MAX + 1);
    off += (SK_AUTH_BOND_LABEL_MAX + 1);
    s->label[SK_AUTH_BOND_LABEL_MAX] = '\0';  // belt + suspenders
    memcpy(&s->paired_at_unix, in + off, 8);
    s->occupied = true;
}

static esp_err_t slot_save(uint8_t slot)
{
    if (slot >= SK_AUTH_BOND_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    char key[16];
    snprintf(key, sizeof(key), NVS_KEY_BOND_FMT, (unsigned)slot);

    if (s_slots[slot].occupied) {
        uint8_t buf[SLOT_RECORD_SIZE];
        slot_record_pack(&s_slots[slot], buf);
        err = nvs_set_blob(h, key, buf, sizeof(buf));
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

static void slot_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_BOND_FMT, (unsigned)i);
        uint8_t buf[SLOT_RECORD_SIZE];
        size_t  sz = sizeof(buf);
        if (nvs_get_blob(h, key, buf, &sz) == ESP_OK && sz == SLOT_RECORD_SIZE) {
            slot_record_unpack(buf, &s_slots[i]);
        }
    }
    nvs_close(h);
}

// One-shot migration: if the legacy "token" key is present and slot 0 is
// empty, write the legacy token into slot 0 with a placeholder peer_id +
// "migrated" label, then erase the legacy key. Idempotent.
static void migrate_legacy_token(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t legacy[SK_AUTH_TOKEN_LEN];
    size_t  sz = sizeof(legacy);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_LEGACY_TOK, legacy, &sz);
    if (err != ESP_OK || sz != SK_AUTH_TOKEN_LEN) {
        nvs_close(h);
        return;
    }

    if (!s_slots[0].occupied) {
        s_slots[0].occupied = true;
        memset(s_slots[0].peer_id, 0, SK_AUTH_PEER_ID_LEN);  // legacy marker
        memcpy(s_slots[0].bond_key, legacy, SK_AUTH_TOKEN_LEN);
        snprintf(s_slots[0].label, sizeof(s_slots[0].label), "migrated");
        s_slots[0].paired_at_unix = 0;
        ESP_LOGI(TAG, "migrating legacy token into bond slot 0");
        // Persist via slot_save below (nvs handle already open elsewhere),
        // but slot_save reopens — we keep this simple and just erase here,
        // then call slot_save outside the close.
    }

    nvs_erase_key(h, NVS_KEY_LEGACY_TOK);
    nvs_commit(h);
    nvs_close(h);
    if (s_slots[0].occupied) (void)slot_save(0);
}

static int find_slot_by_peer(const uint8_t peer_id[SK_AUTH_PEER_ID_LEN])
{
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        if (!s_slots[i].occupied) continue;
        if (memcmp(s_slots[i].peer_id, peer_id, SK_AUTH_PEER_ID_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_first_free_slot(void)
{
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        if (!s_slots[i].occupied) return (int)i;
    }
    return -1;
}

// -- Public bond API -------------------------------------------------------

esp_err_t sk_auth_bond_add(const uint8_t peer_id[SK_AUTH_PEER_ID_LEN],
                           const uint8_t bond_key[SK_AUTH_TOKEN_LEN],
                           const char    *label,
                           uint8_t       *out_slot)
{
    if (!peer_id || !bond_key) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    int slot = find_slot_by_peer(peer_id);
    if (slot < 0) slot = find_first_free_slot();
    if (slot < 0) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;  // BOND_STORE_FULL
    }

    s_slots[slot].occupied = true;
    memcpy(s_slots[slot].peer_id,  peer_id,  SK_AUTH_PEER_ID_LEN);
    memcpy(s_slots[slot].bond_key, bond_key, SK_AUTH_TOKEN_LEN);
    if (label) {
        strncpy(s_slots[slot].label, label, SK_AUTH_BOND_LABEL_MAX);
        s_slots[slot].label[SK_AUTH_BOND_LABEL_MAX] = '\0';
    } else {
        s_slots[slot].label[0] = '\0';
    }
    time_t now = 0;
    time(&now);
    s_slots[slot].paired_at_unix = (int64_t)now;

    esp_err_t err = slot_save((uint8_t)slot);
    xSemaphoreGive(s_mtx);

    if (err == ESP_OK) {
        if (out_slot) *out_slot = (uint8_t)slot;
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"slot\":%u}", (unsigned)slot);
        sk_event_bus_publish("auth.bond.added", payload);
        ESP_LOGI(TAG, "bond installed in slot %u", (unsigned)slot);
        SK_LOG_I("auth", "bond.added",
                 "slot=%u peer=%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)slot,
                 peer_id[0], peer_id[1], peer_id[2],
                 peer_id[3], peer_id[4], peer_id[5]);
    }
    return err;
}

esp_err_t sk_auth_bond_remove(uint8_t slot)
{
    if (slot >= SK_AUTH_BOND_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_slots[slot].occupied) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
    if (s_active_slot == slot) {
        s_active_set  = false;
        s_active_slot = SK_AUTH_BOND_SLOT_INVALID;
        memset(s_active_key, 0, sizeof(s_active_key));
    }
    esp_err_t err = slot_save(slot);
    xSemaphoreGive(s_mtx);
    if (err == ESP_OK) {
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"slot\":%u}", (unsigned)slot);
        sk_event_bus_publish("auth.bond.removed", payload);
        SK_LOG_I("auth", "bond.removed", "slot=%u", (unsigned)slot);
    }
    return err;
}

esp_err_t sk_auth_bond_list(sk_auth_bond_info_t out[SK_AUTH_BOND_SLOT_COUNT],
                            uint8_t *out_count)
{
    if (!out || !out_count) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint8_t n = 0;
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        if (!s_slots[i].occupied) continue;
        out[n].slot = i;
        memcpy(out[n].peer_id, s_slots[i].peer_id, SK_AUTH_PEER_ID_LEN);
        memcpy(out[n].label,   s_slots[i].label,   SK_AUTH_BOND_LABEL_MAX + 1);
        out[n].paired_at_unix = s_slots[i].paired_at_unix;
        n++;
    }
    xSemaphoreGive(s_mtx);
    *out_count = n;
    return ESP_OK;
}

esp_err_t sk_auth_bond_lookup(const uint8_t peer_id[SK_AUTH_PEER_ID_LEN],
                              uint8_t       bond_key_out[SK_AUTH_TOKEN_LEN],
                              uint8_t      *slot_out)
{
    if (!peer_id) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int slot = find_slot_by_peer(peer_id);
    if (slot < 0) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    if (bond_key_out) memcpy(bond_key_out, s_slots[slot].bond_key, SK_AUTH_TOKEN_LEN);
    if (slot_out)     *slot_out = (uint8_t)slot;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

uint8_t sk_auth_bond_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        if (s_slots[i].occupied) n++;
    }
    return n;
}

// -- Active session bond ---------------------------------------------------

const uint8_t *sk_auth_active_bond_key(void)
{
    return s_active_set ? s_active_key : NULL;
}

uint8_t sk_auth_active_bond_slot(void)
{
    return s_active_slot;
}

void sk_auth_active_bond_clear(void)
{
    s_active_set  = false;
    s_active_slot = SK_AUTH_BOND_SLOT_INVALID;
    memset(s_active_key, 0, sizeof(s_active_key));
}

// Internal: called by handshake_verify_peer when a slot's key matched.
void sk_auth__activate_slot(uint8_t slot)
{
    if (slot >= SK_AUTH_BOND_SLOT_COUNT) return;
    if (!s_slots[slot].occupied) return;
    memcpy(s_active_key, s_slots[slot].bond_key, SK_AUTH_TOKEN_LEN);
    s_active_set  = true;
    s_active_slot = slot;
}

// Internal: handshake_verify_peer iterates slots through this read accessor
// (so all slot inspection stays in sk_auth.c, sibling files don't touch
// s_slots directly).
bool sk_auth__slot_get_key(uint8_t slot, uint8_t out[SK_AUTH_TOKEN_LEN])
{
    if (slot >= SK_AUTH_BOND_SLOT_COUNT) return false;
    if (!s_slots[slot].occupied) return false;
    memcpy(out, s_slots[slot].bond_key, SK_AUTH_TOKEN_LEN);
    return true;
}

// Legacy install_token: ECDH complete still calls this in the unmodified
// (no-passphrase, no peer_id) path. Maps the install onto slot 0 with a
// zeroed peer_id placeholder. Step 4 (pairing-time enforcement) replaces
// callers with the new sk_auth_bond_add path that carries a real peer_id.
esp_err_t sk_auth__install_token(const uint8_t token[SK_AUTH_TOKEN_LEN])
{
    if (!token) return ESP_ERR_INVALID_ARG;
    uint8_t zero_peer[SK_AUTH_PEER_ID_LEN] = {0};
    uint8_t slot = 0;
    esp_err_t err = sk_auth_bond_add(zero_peer, token, "legacy-pairing", &slot);
    if (err == ESP_OK) {
        sk_event_bus_publish("auth.token.installed", NULL);
        ESP_LOGI(TAG, "session token installed (legacy path, slot %u)", (unsigned)slot);
    }
    return err;
}

bool sk_auth_has_bond(void)
{
    return sk_auth_bond_count() > 0;
}

esp_err_t sk_auth_clear_all(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    // Snapshot which slots were occupied so we can log each removal after
    // wiping. Done under the mutex so the snapshot is consistent.
    bool was_occupied[SK_AUTH_BOND_SLOT_COUNT];
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        was_occupied[i] = s_slots[i].occupied;
    }
    memset(s_slots, 0, sizeof(s_slots));
    s_active_set  = false;
    s_active_slot = SK_AUTH_BOND_SLOT_INVALID;
    memset(s_active_key, 0, sizeof(s_active_key));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_mtx);
    sk_event_bus_publish("auth.bond.revoked", "{\"reason\":\"clear\"}");
    ESP_LOGW(TAG, "auth cleared (factory / unpair)");
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        if (was_occupied[i]) {
            SK_LOG_I("auth", "bond.removed", "slot=%u", (unsigned)i);
        }
    }
    return ESP_OK;
}

esp_err_t sk_auth_rotate_token(void)
{
    uint8_t fresh[SK_AUTH_TOKEN_LEN];
    esp_fill_random(fresh, sizeof(fresh));

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    // Rotate the active slot if there is one; otherwise slot 0 (legacy).
    uint8_t target = s_active_set ? s_active_slot : 0;
    if (target >= SK_AUTH_BOND_SLOT_COUNT || !s_slots[target].occupied) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_slots[target].bond_key, fresh, SK_AUTH_TOKEN_LEN);
    if (s_active_set && s_active_slot == target) {
        memcpy(s_active_key, fresh, SK_AUTH_TOKEN_LEN);
    }
    esp_err_t err = slot_save(target);
    xSemaphoreGive(s_mtx);

    if (err == ESP_OK) sk_event_bus_publish("auth.token.rotated", NULL);
    return err;
}

// -- Pairing mode ----------------------------------------------------------

static void pair_timeout_cb(void *arg)
{
    (void)arg;
    sk_auth_close_pairing_mode("timeout");
}

sk_auth_pairing_state_t sk_auth_pairing_state(void) { return s_pairing; }

esp_err_t sk_auth_open_pairing_mode(uint32_t timeout_sec)
{
    if (s_pairing == SK_AUTH_PAIRING_OPEN) return ESP_ERR_INVALID_STATE;
    if (timeout_sec == 0) timeout_sec = 60;
    if (!s_pair_timer) {
        const esp_timer_create_args_t args = {
            .callback = pair_timeout_cb,
            .name     = "sk_auth_pair",
        };
        esp_err_t err = esp_timer_create(&args, &s_pair_timer);
        if (err != ESP_OK) return err;
    }
    s_pairing = SK_AUTH_PAIRING_OPEN;
    esp_timer_start_once(s_pair_timer, (uint64_t)timeout_sec * 1000000ULL);
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"timeout_sec\":%lu}", (unsigned long)timeout_sec);
    sk_event_bus_publish("pairing.mode.open", payload);
    ESP_LOGI(TAG, "pairing mode open (%lus)", (unsigned long)timeout_sec);
    SK_LOG_I("pairing", "open", "ttl=%lu", (unsigned long)timeout_sec);
    return ESP_OK;
}

void sk_auth_close_pairing_mode(const char *reason)
{
    if (s_pairing != SK_AUTH_PAIRING_OPEN) return;
    s_pairing = SK_AUTH_PAIRING_IDLE;
    if (s_pair_timer) esp_timer_stop(s_pair_timer);
    // Pairing window closes → any pending (passphrase-gated) bond is dropped.
    // Lockout window stays — fail counts persist in sk_passphrase NVS.
    pending_clear();
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"reason\":\"%s\"}", reason ? reason : "unknown");
    sk_event_bus_publish("pairing.mode.close", payload);
    ESP_LOGI(TAG, "pairing mode closed: %s", reason ? reason : "?");
    SK_LOG_I("pairing", "close", "reason=%s", reason ? reason : "unknown");
}

// -- Transport-agnostic pairing line dispatcher --------------------------------

static const char HEX[] = "0123456789abcdef";

static void bytes_to_hex_local(const uint8_t *bytes, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = HEX[(bytes[i] >> 4) & 0xF];
        out[2 * i + 1] = HEX[ bytes[i]       & 0xF];
    }
    out[2 * n] = '\0';
}

static int hex_nibble_local(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_to_bytes_local(const char *hex, size_t expect_bytes, uint8_t *out)
{
    if (!hex || strlen(hex) != expect_bytes * 2) return false;
    for (size_t i = 0; i < expect_bytes; i++) {
        int h = hex_nibble_local(hex[2 * i]);
        int l = hex_nibble_local(hex[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

static void emit_err_json(sk_auth_pairing_writer_t writer, void *user,
                          const char *code)
{
    if (!writer) return;
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"ok\":false,\"err\":\"%s\"}\n", code);
    if (n > 0) writer(buf, (size_t)n, user);
}

// Build the JSON list of currently occupied bond slots, used in
// BOND_STORE_FULL responses so SKAPP can present a peer-removal UI.
static void emit_bond_store_full(sk_auth_pairing_writer_t writer, void *user)
{
    char buf[512];
    int  o = 0;
    o += snprintf(buf + o, sizeof(buf) - o,
                  "{\"ok\":false,\"err\":\"ERR_BOND_STORE_FULL\",\"params\":{\"peers\":[");
    bool first = true;
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT && (size_t)o < sizeof(buf); i++) {
        if (!s_slots[i].occupied) continue;
        char peer_hex[SK_AUTH_PEER_ID_LEN * 2 + 1];
        bytes_to_hex_local(s_slots[i].peer_id, SK_AUTH_PEER_ID_LEN, peer_hex);
        // Escape backslashes/quotes lightly: labels are ASCII-controlled
        // upstream; cJSON would be heavier here.
        o += snprintf(buf + o, sizeof(buf) - o,
                      "%s{\"slot\":%u,\"peer_id\":\"%s\",\"label\":\"%s\","
                      "\"paired_at\":%lld}",
                      first ? "" : ",",
                      (unsigned)i, peer_hex,
                      s_slots[i].label,
                      (long long)s_slots[i].paired_at_unix);
        first = false;
    }
    o += snprintf(buf + o, sizeof(buf) - o, "]}}\n");
    if (o > 0 && writer) writer(buf, (size_t)o, user);
}

// Commit the pending bond into the slot store. On success, clears the
// pending RAM state and closes the pairing window.
static esp_err_t pending_commit_locked(uint8_t *out_slot)
{
    if (!s_pending.active) return ESP_ERR_INVALID_STATE;
    esp_err_t err = sk_auth_bond_add(s_pending.peer_id,
                                     s_pending.bond_key,
                                     s_pending.label[0] ? s_pending.label : NULL,
                                     out_slot);
    pending_clear();
    return err;
}

// Handle pairing.ecdh.exchange. peer_id is optional; if absent we use the
// peer's ephemeral public key fingerprint as a stable-enough id (zero peer
// is reserved for the legacy migration slot, never collides).
static sk_auth_pairing_result_t handle_ecdh_exchange(cJSON *msg,
                                                    sk_auth_pairing_writer_t writer,
                                                    void *user)
{
    cJSON *args = cJSON_GetObjectItemCaseSensitive(msg, "args");
    cJSON *pub_node     = args ? cJSON_GetObjectItemCaseSensitive(args, "peer_pub") : NULL;
    cJSON *peer_id_node = args ? cJSON_GetObjectItemCaseSensitive(args, "peer_id") : NULL;
    cJSON *label_node   = args ? cJSON_GetObjectItemCaseSensitive(args, "label")   : NULL;

    if (!cJSON_IsString(pub_node)) {
        emit_err_json(writer, user, "ERR_MISSING_ARG");
        return SK_AUTH_PAIRING_ERR;
    }

    uint8_t peer_pub[SK_AUTH_ECDH_PUBLIC_LEN];
    if (!hex_to_bytes_local(pub_node->valuestring,
                            SK_AUTH_ECDH_PUBLIC_LEN, peer_pub)) {
        emit_err_json(writer, user, "ERR_INVALID_ARG");
        return SK_AUTH_PAIRING_ERR;
    }

    // peer_id: 16 bytes hex. If absent, fall back to a deterministic
    // derivation from the peer_pub (truncated SHA was overkill — we just
    // memcpy the first 16B of peer_pub which is already random).
    uint8_t peer_id[SK_AUTH_PEER_ID_LEN];
    if (cJSON_IsString(peer_id_node) &&
        hex_to_bytes_local(peer_id_node->valuestring, SK_AUTH_PEER_ID_LEN, peer_id)) {
        // ok
    } else {
        memcpy(peer_id, peer_pub, SK_AUTH_PEER_ID_LEN);
    }

    char label[SK_AUTH_BOND_LABEL_MAX + 1] = {0};
    if (cJSON_IsString(label_node)) {
        strncpy(label, label_node->valuestring, SK_AUTH_BOND_LABEL_MAX);
        label[SK_AUTH_BOND_LABEL_MAX] = '\0';
    }

    // Slot accounting — re-pair from the same peer_id reuses its slot, so
    // it does not count against the bond_count. Only block when it would
    // genuinely allocate a new slot.
    bool reuse = false;
    for (uint8_t i = 0; i < SK_AUTH_BOND_SLOT_COUNT; i++) {
        if (s_slots[i].occupied &&
            memcmp(s_slots[i].peer_id, peer_id, SK_AUTH_PEER_ID_LEN) == 0) {
            reuse = true;
            break;
        }
    }
    if (!reuse && sk_auth_bond_count() >= SK_AUTH_BOND_SLOT_COUNT) {
        emit_bond_store_full(writer, user);
        return SK_AUTH_PAIRING_ERR;
    }

    sk_auth_ecdh_ctx_t ctx;
    if (sk_auth_ecdh_begin(&ctx) != ESP_OK) {
        emit_err_json(writer, user, "ERR_INTERNAL");
        return SK_AUTH_PAIRING_ERR;
    }

    uint8_t derived[SK_AUTH_TOKEN_LEN];
    if (sk_auth_ecdh_derive(&ctx, peer_pub, derived) != ESP_OK) {
        emit_err_json(writer, user, "ERR_INTERNAL");
        return SK_AUTH_PAIRING_ERR;
    }

    char our_pub_hex[SK_AUTH_ECDH_PUBLIC_LEN * 2 + 1];
    bytes_to_hex_local(ctx.our_public, SK_AUTH_ECDH_PUBLIC_LEN, our_pub_hex);

    sk_pass_mode_t pmode = sk_passphrase_get_mode();
    bool gate = sk_passphrase_is_set() &&
                (pmode.pairing_required || pmode.always_required);

    if (gate) {
        // Defer the commit. The peer must follow up with
        // pairing.passphrase.verify within the pairing window. We hold
        // the derived key, peer_id, and label in RAM only; if the window
        // closes (timeout / explicit close / wrong passphrase lockout
        // factory-resets), the data is wiped.
        memcpy(s_pending.bond_key, derived, SK_AUTH_TOKEN_LEN);
        memcpy(s_pending.peer_id, peer_id, SK_AUTH_PEER_ID_LEN);
        strncpy(s_pending.label, label, SK_AUTH_BOND_LABEL_MAX);
        s_pending.label[SK_AUTH_BOND_LABEL_MAX] = '\0';
        s_pending.expires_us = 0;   // pairing window owns the timeout
        s_pending.active     = true;

        memset(derived, 0, sizeof(derived));

        char reply[192];
        int n = snprintf(reply, sizeof(reply),
                         "{\"ok\":true,\"data\":{\"our_pub\":\"%s\","
                         "\"need_passphrase\":true,\"attempts_left\":%u}}\n",
                         our_pub_hex,
                         (unsigned)sk_passphrase_attempts_left());
        if (n > 0) writer(reply, (size_t)n, user);
        ESP_LOGI(TAG, "ECDH derived; awaiting pairing.passphrase.verify");
        return SK_AUTH_PAIRING_OK;
    }

    // No gate — commit immediately to the proper slot using peer_id.
    uint8_t slot = 0;
    esp_err_t err = sk_auth_bond_add(peer_id, derived,
                                     label[0] ? label : NULL, &slot);
    memset(derived, 0, sizeof(derived));
    if (err == ESP_ERR_NO_MEM) {
        emit_bond_store_full(writer, user);
        return SK_AUTH_PAIRING_ERR;
    }
    if (err != ESP_OK) {
        emit_err_json(writer, user, "ERR_INTERNAL");
        return SK_AUTH_PAIRING_ERR;
    }

    char reply[160];
    int n = snprintf(reply, sizeof(reply),
                     "{\"ok\":true,\"data\":{\"our_pub\":\"%s\",\"slot\":%u}}\n",
                     our_pub_hex, (unsigned)slot);
    if (n > 0) writer(reply, (size_t)n, user);

    sk_auth_close_pairing_mode("ecdh_complete");
    ESP_LOGI(TAG, "ECDH pairing committed to slot %u", (unsigned)slot);
    return SK_AUTH_PAIRING_OK;
}

// Handle pairing.passphrase.verify (only valid while a pending bond is
// awaiting). Verifies passphrase via sk_passphrase; on success commits the
// pending bond into a slot and closes the pairing window. Lockout (10 wrong)
// is enforced inside sk_passphrase, which publishes the factory-reset event.
static sk_auth_pairing_result_t handle_passphrase_verify(cJSON *msg,
                                                        sk_auth_pairing_writer_t writer,
                                                        void *user)
{
    if (!s_pending.active) {
        emit_err_json(writer, user, "ERR_NO_PENDING_BOND");
        return SK_AUTH_PAIRING_ERR;
    }

    cJSON *args = cJSON_GetObjectItemCaseSensitive(msg, "args");
    cJSON *plain = args ? cJSON_GetObjectItemCaseSensitive(args, "plain") : NULL;
    if (!cJSON_IsString(plain)) {
        emit_err_json(writer, user, "ERR_MISSING_ARG");
        return SK_AUTH_PAIRING_ERR;
    }

    uint8_t left = 0;
    esp_err_t err = sk_passphrase_verify(plain->valuestring, &left);
    if (err == ESP_OK) {
        uint8_t slot = 0;
        esp_err_t cerr = pending_commit_locked(&slot);
        if (cerr == ESP_ERR_NO_MEM) {
            emit_bond_store_full(writer, user);
            return SK_AUTH_PAIRING_ERR;
        }
        if (cerr != ESP_OK) {
            emit_err_json(writer, user, "ERR_INTERNAL");
            return SK_AUTH_PAIRING_ERR;
        }
        char reply[96];
        int n = snprintf(reply, sizeof(reply),
                         "{\"ok\":true,\"data\":{\"slot\":%u,\"unlocked\":true}}\n",
                         (unsigned)slot);
        if (n > 0) writer(reply, (size_t)n, user);
        sk_auth_close_pairing_mode("ecdh_complete");
        return SK_AUTH_PAIRING_OK;
    }

    if (err == ESP_ERR_INVALID_RESPONSE) {
        char reply[128];
        int n = snprintf(reply, sizeof(reply),
                         "{\"ok\":false,\"err\":\"ERR_PASSPHRASE_INCORRECT\","
                         "\"params\":{\"attempts_left\":%u}}\n",
                         (unsigned)left);
        if (n > 0) writer(reply, (size_t)n, user);
        // Pending stays — peer may retry within the pairing window.
        // Lockout (left==0 + sk_passphrase published factory-reset)
        // wipes everything via the standard event path.
        return SK_AUTH_PAIRING_ERR;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        // Passphrase was cleared between exchange and verify (race). Commit
        // the pending bond as if no gate existed — the security policy
        // tracks the *current* state, not the snapshot at exchange time.
        uint8_t slot = 0;
        esp_err_t cerr = pending_commit_locked(&slot);
        if (cerr != ESP_OK) {
            emit_err_json(writer, user, "ERR_INTERNAL");
            return SK_AUTH_PAIRING_ERR;
        }
        char reply[96];
        int n = snprintf(reply, sizeof(reply),
                         "{\"ok\":true,\"data\":{\"slot\":%u,\"unlocked\":true,\"set\":false}}\n",
                         (unsigned)slot);
        if (n > 0) writer(reply, (size_t)n, user);
        sk_auth_close_pairing_mode("ecdh_complete");
        return SK_AUTH_PAIRING_OK;
    }

    emit_err_json(writer, user, "ERR_INTERNAL");
    return SK_AUTH_PAIRING_ERR;
}

sk_auth_pairing_result_t sk_auth_pairing_dispatch_line(
    const char *line,
    size_t      len,
    sk_auth_pairing_writer_t writer,
    void       *writer_user)
{
    (void)len;
    if (!line || !writer) return SK_AUTH_PAIRING_ERR;

    if (s_pairing != SK_AUTH_PAIRING_OPEN) {
        emit_err_json(writer, writer_user, "ERR_PAIRING_NOT_OPEN");
        return SK_AUTH_PAIRING_NOT_OPEN;
    }

    cJSON *msg = cJSON_Parse(line);
    if (!msg) {
        emit_err_json(writer, writer_user, "ERR_INVALID_ARG");
        return SK_AUTH_PAIRING_ERR;
    }

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(msg, "cmd");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(msg);
        emit_err_json(writer, writer_user, "ERR_UNKNOWN_COMMAND");
        return SK_AUTH_PAIRING_ERR;
    }

    sk_auth_pairing_result_t r;
    if (strcmp(cmd->valuestring, "pairing.ecdh.exchange") == 0) {
        r = handle_ecdh_exchange(msg, writer, writer_user);
    } else if (strcmp(cmd->valuestring, "pairing.passphrase.verify") == 0) {
        r = handle_passphrase_verify(msg, writer, writer_user);
    } else {
        emit_err_json(writer, writer_user, "ERR_UNKNOWN_COMMAND");
        r = SK_AUTH_PAIRING_ERR;
    }
    cJSON_Delete(msg);
    return r;
}

// -- CLI commands ----------------------------------------------------------

static sk_err_t cmd_pairing_status(sk_cli_ctx_t *ctx)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"mode\":\"%s\",\"bonded\":%s,\"slots_used\":%u}",
             s_pairing == SK_AUTH_PAIRING_OPEN ? "open" : "idle",
             sk_auth_has_bond() ? "true" : "false",
             (unsigned)sk_auth_bond_count());
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static sk_err_t cmd_pairing_start(sk_cli_ctx_t *ctx)
{
    esp_err_t e = sk_auth_open_pairing_mode(60);
    if (e == ESP_ERR_INVALID_STATE) { sk_cli_err(ctx, SK_ERR_BUSY, NULL); return SK_OK; }
    sk_cli_ok(ctx, "{\"mode\":\"open\",\"timeout_sec\":60}");
    return SK_OK;
}

static sk_err_t cmd_pairing_stop(sk_cli_ctx_t *ctx)
{
    sk_auth_close_pairing_mode("user");
    sk_cli_ok(ctx, "{\"mode\":\"idle\"}");
    return SK_OK;
}

static sk_err_t cmd_ble_unpair(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    sk_auth_clear_all();
    sk_cli_ok(ctx, "{\"unpaired\":true}");
    return SK_OK;
}

static sk_err_t cmd_auth_token_rotate(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    sk_auth_rotate_token();
    sk_cli_ok(ctx, "{\"rotated\":true}");
    return SK_OK;
}

static sk_err_t cmd_confirm_get(sk_cli_ctx_t *ctx)
{
    char hex[SK_AUTH_CONFIRM_TOKEN_LEN + 1];
    uint32_t ttl = 0;
    if (sk_auth_confirm_issue(hex, &ttl) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, NULL);
        return SK_OK;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"token\":\"%s\",\"ttl_sec\":%lu}", hex, (unsigned long)ttl);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static sk_err_t cmd_bond_list(sk_cli_ctx_t *ctx)
{
    sk_auth_bond_info_t list[SK_AUTH_BOND_SLOT_COUNT];
    uint8_t n = 0;
    if (sk_auth_bond_list(list, &n) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, NULL);
        return SK_OK;
    }

    // Streamed JSON build to keep the buffer tight.
    char buf[640];
    int  o = 0;
    o += snprintf(buf + o, sizeof(buf) - o,
                  "{\"count\":%u,\"capacity\":%u,\"active_slot\":%d,\"peers\":[",
                  (unsigned)n,
                  (unsigned)SK_AUTH_BOND_SLOT_COUNT,
                  s_active_set ? (int)s_active_slot : -1);
    for (uint8_t i = 0; i < n && (size_t)o < sizeof(buf); i++) {
        char peer_hex[SK_AUTH_PEER_ID_LEN * 2 + 1];
        bytes_to_hex_local(list[i].peer_id, SK_AUTH_PEER_ID_LEN, peer_hex);
        o += snprintf(buf + o, sizeof(buf) - o,
                      "%s{\"slot\":%u,\"peer_id\":\"%s\",\"label\":\"%s\","
                      "\"paired_at\":%lld}",
                      i == 0 ? "" : ",",
                      (unsigned)list[i].slot,
                      peer_hex,
                      list[i].label,
                      (long long)list[i].paired_at_unix);
    }
    o += snprintf(buf + o, sizeof(buf) - o, "]}");
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static sk_err_t cmd_bond_remove(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    long slot = -1;
    if (!sk_cli_arg_long(ctx, "slot", &slot) || slot < 0 ||
        slot >= SK_AUTH_BOND_SLOT_COUNT) {
        sk_cli_err(ctx, SK_ERR_INVALID_ARG, NULL);
        return SK_OK;
    }
    esp_err_t err = sk_auth_bond_remove((uint8_t)slot);
    if (err == ESP_ERR_NOT_FOUND) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, "{\"reason\":\"slot_empty\"}");
        return SK_OK;
    }
    if (err != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, NULL);
        return SK_OK;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"slot\":%ld,\"removed\":true}", slot);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name = "pairing.status",
      .summary = "Show pairing window state and how many bond slots are occupied",
      .usage   = "pairing status",
      .help_block =
          "Reports a snapshot:\n"
          "  mode         'open' if the 60-second pairing window is active\n"
          "               (button short-press / pairing.start), 'idle' else\n"
          "  bonded       true if at least one SKAPP install is paired\n"
          "  slots_used   number of occupied bond slots (0..8)\n"
          "\n"
          "Read-only.\n"
          "\n"
          "Example:\n"
          "  pairing status",
      .handler = cmd_pairing_status },

    { .name = "pairing.start",
      .summary = "Open a 60-second BLE pairing window for SKAPP to discover the device",
      .usage   = "pairing start",
      .help_block =
          "Opens a 60-second window during which the device advertises over\n"
          "BLE and accepts a new pairing handshake from SKAPP. The window\n"
          "auto-closes after 60s if nothing happens.\n"
          "\n"
          "Manual CLI use is rarely needed: a SHORT_PRESS on the physical\n"
          "button does exactly the same thing.\n"
          "\n"
          "Example:\n"
          "  pairing start",
      .handler = cmd_pairing_start },

    { .name = "pairing.stop",
      .summary = "Close the pairing window early",
      .usage   = "pairing stop",
      .help_block =
          "Closes the BLE pairing window before its 60-second auto-timeout.\n"
          "Useful if you've already paired and want to stop advertising\n"
          "sooner (slightly lower power use, smaller attack surface).",
      .handler = cmd_pairing_stop },

    { .name = "ble.unpair",
      .summary = "Forget every paired phone (clears all bond slots)",
      .usage   = "ble unpair",
      .critical = true,
      .help_block =
          "Wipes every stored bond slot and invalidates the active session.\n"
          "The device immediately disconnects. Pair a phone again with\n"
          "`pairing start` (or short-press the button).\n"
          "\n"
          "Use when: you want to revoke every SKAPP at once. To remove a\n"
          "single peer, use `bond.remove <slot>` (lands in step 6).\n"
          "\n"
          "Critical — see `help device.confirm-token` for the confirm flow.",
      .handler = cmd_ble_unpair },

    { .name = "auth.token.rotate",
      .summary = "Rotate the active session token (SKAPP-only)",
      .usage   = "auth token rotate",
      .critical = true,
      .hidden   = true,
      .help_block =
          "Invalidates the active session HMAC token and issues a new one\n"
          "without breaking the BLE bond. Only the active slot's key is\n"
          "rotated; other paired phones keep working.",
      .handler = cmd_auth_token_rotate },

    { .name = "bond.list",
      .summary = "List paired SKAPP installs (slot, peer_id, label, paired_at)",
      .usage   = "bond list",
      .requires_auth = true,
      .help_block =
          "Returns every occupied bond slot. The returned `active_slot` is\n"
          "the slot whose key signed the current authenticated session\n"
          "(-1 if none yet).\n"
          "\n"
          "SKAPP uses this to populate the 'Eşleşmiş SKAPP'lar' list and\n"
          "the BOND_STORE_FULL slot-picker dialog.",
      .handler = cmd_bond_list },

    { .name = "bond.remove",
      .summary = "Remove a paired peer by slot (forgets that SKAPP install)",
      .usage   = "bond remove --slot <0..7>",
      .critical = true,
      .requires_auth = true,
      .help_block =
          "Wipes the bond at the given slot. The disconnected peer must\n"
          "re-pair to regain access. If you remove the slot of the active\n"
          "session, the device drops the connection too.",
      .handler = cmd_bond_remove },

    { .name = "device.confirm-token",
      .summary = "Issue a single-use token to authorize a critical command",
      .usage   = "device confirm-token",
      .help_block =
          "Critical commands (anything tagged 'critical' — wifi.forget,\n"
          "device.factory-reset, ota.update, etc.) require a confirm token.\n"
          "\n"
          "Easy way (recommended for humans):\n"
          "  Just call your critical command. The device refuses, generates\n"
          "  a fresh token, and prints the exact retry command for you to\n"
          "  copy:\n"
          "\n"
          "    bf> device factory-reset\n"
          "    error: ERR_CONFIRM_REQUIRED — to confirm, copy/paste this within 30s:\n"
          "           device factory-reset --confirm-token a3f81c\n"
          "    bf> device factory-reset --confirm-token a3f81c\n"
          "    ok.\n"
          "\n"
          "Pre-fetch way (for scripts and SKAPP):\n"
          "  Call this command first to grab a token, then chain it onto\n"
          "  any critical command yourself:\n"
          "\n"
          "    bf> device confirm-token\n"
          "    ok. {\"token\":\"a3f81c\",\"ttl_sec\":30}\n"
          "    bf> device factory-reset --confirm-token a3f81c\n"
          "\n"
          "Each token is single-use.",
      .handler = cmd_confirm_get },
};

static void on_control_short_press(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    if (s_pairing == SK_AUTH_PAIRING_OPEN) return;
    sk_auth_open_pairing_mode(60);
}

static void on_factory_reset_requested(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    ESP_LOGW(TAG, "factory reset requested — clearing all bond slots");
    sk_auth_clear_all();
}

static esp_err_t auth_confirm_issuer_bridge(char *out_hex, size_t out_size,
                                             uint32_t *out_ttl_sec)
{
    if (!out_hex || out_size <= SK_AUTH_CONFIRM_TOKEN_LEN) return ESP_ERR_INVALID_SIZE;
    return sk_auth_confirm_issue(out_hex, out_ttl_sec);
}

esp_err_t sk_auth_init(void)
{
    if (s_mtx) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;

    memset(s_slots, 0, sizeof(s_slots));
    memset(s_active_key, 0, sizeof(s_active_key));
    s_active_set  = false;
    s_active_slot = SK_AUTH_BOND_SLOT_INVALID;

    slot_load_all();
    migrate_legacy_token();

    for (size_t i = 0; i < sizeof(s_cmds)/sizeof(s_cmds[0]); i++) {
        sk_cli_register(&s_cmds[i]);
    }
    sk_cli_set_confirm_issuer(auth_confirm_issuer_bridge);
    int sub;
    sk_event_bus_subscribe("control.short-press",            on_control_short_press,     NULL, &sub);
    sk_event_bus_subscribe("device.factory-reset.requested", on_factory_reset_requested, NULL, &sub);
    sk_capabilities_register_book("sk_auth", "0.2.0");

    bool any = sk_auth_has_bond();
    if (!any) {
        ESP_LOGI(TAG, "no bond stored — opening 60s pairing window");
        sk_auth_open_pairing_mode(60);
    }

    ESP_LOGI(TAG, "sk_auth ready (bonds=%u/%u)",
             (unsigned)sk_auth_bond_count(),
             (unsigned)SK_AUTH_BOND_SLOT_COUNT);
    return ESP_OK;
}
