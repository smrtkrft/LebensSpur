#include "sk_auth.h"
#include "sk_log.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"

static const char *TAG = "sk_auth_hs";

extern bool sk_auth__has_token(void);
extern bool sk_auth__slot_get_key(uint8_t slot, uint8_t out[SK_AUTH_TOKEN_LEN]);
extern void sk_auth__activate_slot(uint8_t slot);

#define HANDSHAKE_TIMEOUT_US  (5LL * 1000 * 1000)

static int hmac_prefix(const uint8_t *token, size_t token_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t out[SK_AUTH_RESPONSE_LEN])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    uint8_t full[32];
    int rc = mbedtls_md_hmac(md, token, token_len, data, data_len, full);
    if (rc != 0) return rc;
    memcpy(out, full, SK_AUTH_RESPONSE_LEN);
    return 0;
}

esp_err_t sk_auth_handshake_begin(sk_auth_handshake_t *hs)
{
    if (!hs) return ESP_ERR_INVALID_ARG;
    // At least one bond must exist for a session to be possible. The active
    // slot will be picked by verify_peer based on which key signs the
    // peer's response.
    if (!sk_auth__has_token()) return ESP_ERR_INVALID_STATE;

    esp_fill_random(hs->our_challenge, sizeof(hs->our_challenge));
    hs->started_us    = esp_timer_get_time();
    hs->peer_verified = false;
    // Begin with no active slot. Reset stale activation from a previous
    // session so HMAC envelope ops fail closed until verify_peer succeeds.
    sk_auth_active_bond_clear();
    return ESP_OK;
}

esp_err_t sk_auth_handshake_verify_peer(sk_auth_handshake_t *hs,
                                        const uint8_t response[SK_AUTH_RESPONSE_LEN])
{
    if (!hs || !response) return ESP_ERR_INVALID_ARG;

    if (esp_timer_get_time() - hs->started_us > HANDSHAKE_TIMEOUT_US) {
        SK_LOG_E("auth", "handshake.fail", "reason=timeout");
        return ESP_ERR_TIMEOUT;
    }

    // Multi-bond: try each occupied slot's key against our_challenge until
    // we find the one that signed the peer's response. Constant-time compare
    // *within* a slot's check so a near-match cannot leak which byte
    // mismatched; iterating across slots is structurally fine since slot
    // identity is not a secret (SKAPP knows its own slot).
    for (uint8_t slot = 0; slot < SK_AUTH_BOND_SLOT_COUNT; slot++) {
        uint8_t key[SK_AUTH_TOKEN_LEN];
        if (!sk_auth__slot_get_key(slot, key)) continue;

        uint8_t expected[SK_AUTH_RESPONSE_LEN];
        int rc = hmac_prefix(key, SK_AUTH_TOKEN_LEN,
                             hs->our_challenge, SK_AUTH_CHALLENGE_LEN,
                             expected);
        // Wipe key copy regardless of rc.
        if (rc != 0) {
            memset(key, 0, sizeof(key));
            continue;
        }

        uint8_t diff = 0;
        for (int i = 0; i < SK_AUTH_RESPONSE_LEN; i++) {
            diff |= expected[i] ^ response[i];
        }
        memset(expected, 0, sizeof(expected));

        if (diff == 0) {
            sk_auth__activate_slot(slot);
            memset(key, 0, sizeof(key));
            hs->peer_verified = true;
            ESP_LOGI(TAG, "handshake matched bond slot %u", (unsigned)slot);
            SK_LOG_I("auth", "session.unlocked", "slot=%u", (unsigned)slot);
            return ESP_OK;
        }
        memset(key, 0, sizeof(key));
    }

    SK_LOG_E("auth", "handshake.fail", "reason=hmac_mismatch");
    return ESP_FAIL;
}

esp_err_t sk_auth_handshake_answer(const uint8_t challenge[SK_AUTH_CHALLENGE_LEN],
                                   uint8_t       out[SK_AUTH_RESPONSE_LEN])
{
    if (!challenge || !out) return ESP_ERR_INVALID_ARG;

    // verify_peer must have run first and activated a slot. Without an
    // active bond, we have no key to sign with — fail closed.
    const uint8_t *active = sk_auth_active_bond_key();
    if (!active) return ESP_ERR_INVALID_STATE;

    int rc = hmac_prefix(active, SK_AUTH_TOKEN_LEN,
                         challenge, SK_AUTH_CHALLENGE_LEN,
                         out);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
