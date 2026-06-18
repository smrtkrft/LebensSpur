#include "sk_auth.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "mbedtls/md.h"

// Symbol defined in sk_auth.c:
extern const uint8_t *sk_auth__token_ptr(void);
extern bool           sk_auth__has_token(void);

// Replay guard: last 64 nonces seen.
//
// Reset on every fresh secure-session handshake (sk_secure_session_reset →
// sk_auth_replay_reset). The C-R handshake exchanges fresh challenges, so
// any captured pre-reset envelope can no longer be replayed.
#define NONCE_WINDOW 64
static uint32_t s_nonces[NONCE_WINDOW];
static int      s_nonce_head = 0;

void sk_auth_replay_reset(void)
{
    for (int i = 0; i < NONCE_WINDOW; i++) s_nonces[i] = 0;
    s_nonce_head = 0;
}

#define TS_WINDOW_SEC          60
// Wall clock güvenilirlik eşiği. time(NULL) bu değerin altındaysa SKAPP
// time.set push'u henüz gelmemiştir — sadece nonce replay guard'a düşeriz.
// Üstündeyse ts_unix ± TS_WINDOW_SEC ENFORCE edilir (replay protection
// yalnız nonce ringe değil zaman penceresine de bağlanır).
#define TS_VALID_THRESHOLD     1700000000  // ~2023-11-15 UTC epoch sec

static bool nonce_seen(uint32_t n)
{
    for (int i = 0; i < NONCE_WINDOW; i++) {
        if (s_nonces[i] == n && n != 0) return true;
    }
    return false;
}

static void nonce_mark(uint32_t n)
{
    s_nonces[s_nonce_head] = n;
    s_nonce_head = (s_nonce_head + 1) % NONCE_WINDOW;
}

static esp_err_t compute_hmac(const char *body, size_t len, uint8_t out[SK_AUTH_HMAC_LEN])
{
    if (!sk_auth__has_token()) return ESP_ERR_INVALID_STATE;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return ESP_FAIL;
    uint8_t full[32];
    int rc = mbedtls_md_hmac(md,
                             sk_auth__token_ptr(), SK_AUTH_TOKEN_LEN,
                             (const uint8_t *)body, len,
                             full);
    if (rc != 0) return ESP_FAIL;
    memcpy(out, full, SK_AUTH_HMAC_LEN);
    return ESP_OK;
}

esp_err_t sk_auth_sign_message(const char *body, size_t len, uint8_t sig[SK_AUTH_HMAC_LEN])
{
    if (!body || !sig) return ESP_ERR_INVALID_ARG;
    return compute_hmac(body, len, sig);
}

esp_err_t sk_auth_verify_message(const char *body, size_t len,
                                 uint32_t nonce, int64_t ts_unix,
                                 const uint8_t sig[SK_AUTH_HMAC_LEN])
{
    if (!body || !sig) return ESP_ERR_INVALID_ARG;
    if (!sk_auth__has_token()) return ESP_ERR_INVALID_STATE;

    // Timestamp window — SKAPP `time.set` push'u sonrası wall clock güvenilir;
    // o noktadan itibaren ts_unix ± TS_WINDOW_SEC enforce edilir. Wall clock
    // henüz set edilmemişse (örn. soğuk boot, SKAPP daha bağlanmadı) sadece
    // nonce replay guard'a düşeriz — bu zayıf ama tek gerçekçi seçenek çünkü
    // ESP32-C6'da RTC battery / SNTP yok.
    // Madde 17 (security audit 2026-05-16) burada kapatıldı.
    {
        time_t now_wall = time(NULL);
        if (now_wall >= (time_t)TS_VALID_THRESHOLD) {
            int64_t diff = (int64_t)now_wall - ts_unix;
            if (diff < 0) diff = -diff;
            if (diff > TS_WINDOW_SEC) return ESP_FAIL;  // ERR_TIMESTAMP_OUT_OF_WINDOW
        }
        // else: wall clock unset → fall through to nonce-only guard
    }

    if (nonce_seen(nonce)) return ESP_FAIL;

    uint8_t expect[SK_AUTH_HMAC_LEN];
    esp_err_t e = compute_hmac(body, len, expect);
    if (e != ESP_OK) return e;

    // Constant-time compare.
    uint8_t diff = 0;
    for (int i = 0; i < SK_AUTH_HMAC_LEN; i++) diff |= expect[i] ^ sig[i];
    if (diff != 0) return ESP_FAIL;

    nonce_mark(nonce);
    return ESP_OK;
}
