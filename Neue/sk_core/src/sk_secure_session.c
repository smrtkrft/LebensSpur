#include "sk_secure_session.h"
#include "sk_passphrase.h"
#include "sk_event_bus.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sk_session";

static const char HEX[] = "0123456789abcdef";

static void bytes_to_hex(const uint8_t *bytes, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = HEX[(bytes[i] >> 4) & 0xF];
        out[2 * i + 1] = HEX[ bytes[i]       & 0xF];
    }
    out[2 * n] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_to_bytes(const char *hex, size_t expect_bytes, uint8_t *out)
{
    if (!hex || strlen(hex) != expect_bytes * 2) return false;
    for (size_t i = 0; i < expect_bytes; i++) {
        int h = hex_nibble(hex[2 * i]);
        int l = hex_nibble(hex[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

esp_err_t sk_secure_session_begin(sk_secure_session_t *s,
                                  sk_session_send_fn   send,
                                  void                *send_user)
{
    if (!s || !send) return ESP_ERR_INVALID_ARG;

    if (!sk_auth_has_bond()) {
        ESP_LOGW(TAG, "no bond; peer must pair first");
        return ESP_ERR_INVALID_STATE;
    }

    memset(s, 0, sizeof(*s));
    s->send      = send;
    s->send_user = send_user;

    esp_err_t err = sk_auth_handshake_begin(&s->hs);
    if (err != ESP_OK) {
        s->state = SK_SESSION_FAILED;
        return err;
    }

    char hex[SK_AUTH_CHALLENGE_LEN * 2 + 1];
    bytes_to_hex(s->hs.our_challenge, SK_AUTH_CHALLENGE_LEN, hex);

    char line[96];
    int n = snprintf(line, sizeof(line),
                     "{\"evt\":\"auth.challenge\",\"data\":\"%s\"}\n", hex);
    if (n > 0) s->send(line, (size_t)n, s->send_user);

    s->state = SK_SESSION_AWAITING_PEER;
    return ESP_OK;
}

sk_session_feed_t sk_secure_session_feed_line(sk_secure_session_t *s,
                                              const char           *line)
{
    if (!s || !line) return SK_SESSION_FEED_AUTH_INVALID;

    // Once authenticated, every line is a normal command — caller dispatches.
    if (s->state == SK_SESSION_AUTHENTICATED) {
        return SK_SESSION_FEED_PASSTHROUGH;
    }
    if (s->state != SK_SESSION_AWAITING_PEER) {
        return SK_SESSION_FEED_AUTH_INVALID;
    }

    cJSON *msg = cJSON_Parse(line);
    if (!msg) return SK_SESSION_FEED_AUTH_INVALID;

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(msg, "cmd");
    if (!cJSON_IsString(cmd) || strcmp(cmd->valuestring, "auth.response") != 0) {
        cJSON_Delete(msg);
        s->state = SK_SESSION_FAILED;
        return SK_SESSION_FEED_AUTH_INVALID;
    }

    cJSON *args      = cJSON_GetObjectItemCaseSensitive(msg, "args");
    cJSON *resp_node = args ? cJSON_GetObjectItemCaseSensitive(args, "response")  : NULL;
    cJSON *chal_node = args ? cJSON_GetObjectItemCaseSensitive(args, "challenge") : NULL;

    if (!cJSON_IsString(resp_node) || !cJSON_IsString(chal_node)) {
        cJSON_Delete(msg);
        s->state = SK_SESSION_FAILED;
        return SK_SESSION_FEED_AUTH_INVALID;
    }

    uint8_t peer_response[SK_AUTH_RESPONSE_LEN];
    uint8_t peer_challenge[SK_AUTH_CHALLENGE_LEN];
    bool ok =
        hex_to_bytes(resp_node->valuestring, SK_AUTH_RESPONSE_LEN,  peer_response) &&
        hex_to_bytes(chal_node->valuestring, SK_AUTH_CHALLENGE_LEN, peer_challenge);

    cJSON_Delete(msg);
    if (!ok) {
        s->state = SK_SESSION_FAILED;
        return SK_SESSION_FEED_AUTH_INVALID;
    }

    // Step 1: peer's response must verify against our_challenge.
    if (sk_auth_handshake_verify_peer(&s->hs, peer_response) != ESP_OK) {
        s->state = SK_SESSION_FAILED;
        ESP_LOGW(TAG, "peer response verification failed");
        return SK_SESSION_FEED_AUTH_INVALID;
    }

    // Step 2: compute our answer to peer's challenge.
    uint8_t our_answer[SK_AUTH_RESPONSE_LEN];
    if (sk_auth_handshake_answer(peer_challenge, our_answer) != ESP_OK) {
        s->state = SK_SESSION_FAILED;
        return SK_SESSION_FEED_AUTH_INVALID;
    }

    char hex[SK_AUTH_RESPONSE_LEN * 2 + 1];
    bytes_to_hex(our_answer, SK_AUTH_RESPONSE_LEN, hex);

    char reply[96];
    int n = snprintf(reply, sizeof(reply),
                     "{\"ok\":true,\"data\":{\"answer\":\"%s\"}}\n", hex);
    if (n > 0) s->send(reply, (size_t)n, s->send_user);

    s->state = SK_SESSION_AUTHENTICATED;
    // Fresh handshake = fresh replay window. SKAPP starts each new
    // CliSigner from nonce=1 (after autoDispose / invalidate the Riverpod
    // session is recreated end-to-end), and without this reset the
    // firmware-side global s_nonces would still hold previous-session
    // values and reject every new request as a replay.
    sk_auth_replay_reset();

    // Content-access passphrase gate. Once C-R has matched a bond slot we
    // know the peer is legitimate at the BLE/TCP layer; the passphrase is
    // a *separate* user-knowledge factor that protects content (notebook,
    // API endpoints, settings). If the user has set one and chosen the
    // 'always_required' mode, hold the session in the LOCKED phase until
    // a successful auth.passphrase.verify arrives — see dispatch_signed.
    sk_pass_mode_t pmode = sk_passphrase_get_mode();
    bool needs_unlock    = sk_passphrase_is_set() && pmode.always_required;
    s->passphrase_unlocked = !needs_unlock;

    if (needs_unlock) {
        uint8_t left = sk_passphrase_attempts_left();
        char ev[80];
        int en = snprintf(ev, sizeof(ev),
                          "{\"evt\":\"auth.passphrase.required\",\"data\":{\"attempts_left\":%u}}\n",
                          (unsigned)left);
        if (en > 0) s->send(ev, (size_t)en, s->send_user);
        ESP_LOGI(TAG, "session authenticated; passphrase gate ON (attempts_left=%u)",
                 (unsigned)left);
    } else {
        ESP_LOGI(TAG, "session authenticated");
    }
    return SK_SESSION_FEED_AUTH_PROGRESSED;
}

bool sk_secure_session_timed_out(const sk_secure_session_t *s)
{
    if (!s) return false;
    if (s->state != SK_SESSION_AWAITING_PEER) return false;
    int64_t now = esp_timer_get_time();
    return (now - s->hs.started_us) > SK_SESSION_HANDSHAKE_TIMEOUT_US;
}

void sk_secure_session_reset(sk_secure_session_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->state = SK_SESSION_FRESH;
}

// -- Per-message HMAC envelope ----------------------------------------------

static void emit_err(sk_cli_writer_t writer, void *user, const char *params_or_null)
{
    char buf[160];
    int n;
    if (params_or_null) {
        n = snprintf(buf, sizeof(buf),
                     "{\"ok\":false,\"err\":\"ERR_HMAC_INVALID\",\"params\":%s}\n",
                     params_or_null);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"ok\":false,\"err\":\"ERR_HMAC_INVALID\"}\n");
    }
    if (n > 0 && writer) writer(buf, (size_t)n, user);
}

// Emit a structured "session is locked, send auth.passphrase.verify" reply.
// We re-use the inner CLI envelope shape so SKAPP can correlate by `id`.
static void emit_session_locked(sk_cli_writer_t writer, void *user,
                                int id, uint8_t attempts_left)
{
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "{\"id\":%d,\"ok\":false,\"err\":\"ERR_SESSION_LOCKED\","
                     "\"params\":{\"attempts_left\":%u}}\n",
                     id, (unsigned)attempts_left);
    if (n > 0 && writer) writer(buf, (size_t)n, user);
}

// Intercepts auth.passphrase.verify before the regular CLI dispatcher sees
// it: the result has to mutate session state (passphrase_unlocked) which
// the CLI layer doesn't know about. Inputs are already HMAC-verified.
// Returns true if the line was handled here (caller skips normal dispatch).
static bool handle_passphrase_verify(sk_secure_session_t *s,
                                     const char *body,
                                     sk_cli_writer_t writer, void *user)
{
    cJSON *inner = cJSON_Parse(body);
    if (!inner) return false;

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(inner, "cmd");
    if (!cJSON_IsString(cmd) ||
        strcmp(cmd->valuestring, "auth.passphrase.verify") != 0) {
        cJSON_Delete(inner);
        return false;
    }

    int id = 0;
    cJSON *id_node = cJSON_GetObjectItemCaseSensitive(inner, "id");
    if (cJSON_IsNumber(id_node)) id = (int)id_node->valuedouble;

    cJSON *args      = cJSON_GetObjectItemCaseSensitive(inner, "args");
    cJSON *plain_node = args ? cJSON_GetObjectItemCaseSensitive(args, "plain") : NULL;
    if (!cJSON_IsString(plain_node)) {
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "{\"id\":%d,\"ok\":false,\"err\":\"ERR_MISSING_ARG\"}\n", id);
        if (n > 0 && writer) writer(buf, (size_t)n, user);
        cJSON_Delete(inner);
        return true;
    }

    uint8_t left = 0;
    esp_err_t err = sk_passphrase_verify(plain_node->valuestring, &left);
    cJSON_Delete(inner);

    if (err == ESP_OK) {
        if (s) s->passphrase_unlocked = true;
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "{\"id\":%d,\"ok\":true,\"data\":{\"unlocked\":true,\"attempts_left\":%u}}\n",
                         id, (unsigned)left);
        if (n > 0 && writer) writer(buf, (size_t)n, user);
        sk_event_bus_publish("auth.passphrase.unlocked", NULL);
        ESP_LOGI(TAG, "session unlocked via passphrase");
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        // Wrong passphrase. sk_passphrase already incremented the persistent
        // fail counter and (at lockout) published device.factory-reset.
        char buf[128];
        int n = snprintf(buf, sizeof(buf),
                         "{\"id\":%d,\"ok\":false,\"err\":\"ERR_PASSPHRASE_INCORRECT\","
                         "\"params\":{\"attempts_left\":%u}}\n",
                         id, (unsigned)left);
        if (n > 0 && writer) writer(buf, (size_t)n, user);
    } else if (err == ESP_ERR_INVALID_STATE) {
        // No passphrase configured — peer is confused. Mark unlocked so the
        // next command isn't blocked spuriously.
        if (s) s->passphrase_unlocked = true;
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "{\"id\":%d,\"ok\":true,\"data\":{\"unlocked\":true,\"set\":false}}\n", id);
        if (n > 0 && writer) writer(buf, (size_t)n, user);
    } else {
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "{\"id\":%d,\"ok\":false,\"err\":\"ERR_INTERNAL\"}\n", id);
        if (n > 0 && writer) writer(buf, (size_t)n, user);
    }
    return true;
}

void sk_secure_session_dispatch_signed(sk_secure_session_t *s,
                                       const char          *line,
                                       sk_cli_writer_t      writer,
                                       void                *user)
{
    if (!line || !writer) return;

    cJSON *env = cJSON_Parse(line);
    if (!env) {
        emit_err(writer, user, "{\"reason\":\"json_parse\"}");
        return;
    }

    cJSON *body_node  = cJSON_GetObjectItemCaseSensitive(env, "body");
    cJSON *sig_node   = cJSON_GetObjectItemCaseSensitive(env, "sig");
    cJSON *nonce_node = cJSON_GetObjectItemCaseSensitive(env, "nonce");
    cJSON *ts_node    = cJSON_GetObjectItemCaseSensitive(env, "ts");

    if (!cJSON_IsString(body_node) || !cJSON_IsString(sig_node) ||
        !cJSON_IsNumber(nonce_node)) {
        cJSON_Delete(env);
        emit_err(writer, user, "{\"reason\":\"missing_fields\"}");
        return;
    }

    uint8_t sig[SK_AUTH_HMAC_LEN];
    if (!hex_to_bytes(sig_node->valuestring, SK_AUTH_HMAC_LEN, sig)) {
        cJSON_Delete(env);
        emit_err(writer, user, "{\"reason\":\"sig_hex\"}");
        return;
    }

    const char *body     = body_node->valuestring;
    size_t      body_len = strlen(body);
    uint32_t    nonce    = (uint32_t)nonce_node->valuedouble;
    int64_t     ts       = cJSON_IsNumber(ts_node) ? (int64_t)ts_node->valuedouble : 0;

    if (sk_auth_verify_message(body, body_len, nonce, ts, sig) != ESP_OK) {
        cJSON_Delete(env);
        emit_err(writer, user, NULL);
        return;
    }

    // HMAC verified. If the passphrase gate is engaged we admit *only*
    // auth.passphrase.verify; everything else gets ERR_SESSION_LOCKED so
    // SKAPP can show its prompt without exposing other commands' params
    // to a thief who somehow stole the bond key.
    bool gate_open = (s == NULL) || s->passphrase_unlocked;

    if (!gate_open) {
        // Try to handle verify here. If it's a different command, refuse.
        if (handle_passphrase_verify(s, body, writer, user)) {
            cJSON_Delete(env);
            return;
        }
        // Pull the inner id (best-effort) so SKAPP can correlate.
        int id = 0;
        cJSON *inner = cJSON_Parse(body);
        if (inner) {
            cJSON *id_node = cJSON_GetObjectItemCaseSensitive(inner, "id");
            if (cJSON_IsNumber(id_node)) id = (int)id_node->valuedouble;
            cJSON_Delete(inner);
        }
        emit_session_locked(writer, user, id, sk_passphrase_attempts_left());
        cJSON_Delete(env);
        return;
    }

    // Gate is open — verify command is also welcome here (SKAPP may send
    // it when the user changes their passphrase mid-session); but normal
    // dispatch on a no-op verify works too, since auth.passphrase.verify
    // will be a registered CLI command in step 6. Intercept anyway to
    // keep gate state consistent.
    if (handle_passphrase_verify(s, body, writer, user)) {
        cJSON_Delete(env);
        return;
    }

    // Verified — dispatch inner body to sk_cli through the *authenticated*
    // entrypoint so that commands marked `requires_auth` (encrypted store,
    // user scratch area) are allowed to run. Note `body` points into the
    // cJSON-owned string, so we hand it off before deleting the envelope.
    sk_cli_dispatch_line_authenticated(body, writer, user);
    cJSON_Delete(env);
}
