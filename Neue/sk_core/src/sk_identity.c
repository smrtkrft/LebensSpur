#include "sk_identity.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"

static const char *TAG = "sk_identity";

// Crockford Base32 alphabet — 32 symbols, no I/L/O/U to avoid label/QR ambiguity.
static const char SK_ID_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static char     s_prefix[SK_IDENTITY_PREFIX_LEN + 1] = {0};
static char     s_hw_rev                              = '\0';
static char     s_unique[SK_IDENTITY_UNIQUE_LEN + 1]  = {0};
static char     s_suffix[SK_IDENTITY_SUFFIX_LEN + 1]  = {0};
static char     s_full[SK_IDENTITY_FULL_LEN + 1]      = {0};
static uint8_t  s_mac[6]                              = {0};
static bool     s_initialized                         = false;

static bool valid_prefix(const char *p)
{
    if (p == NULL) return false;
    if (strlen(p) != SK_IDENTITY_PREFIX_LEN) return false;
    for (int i = 0; i < SK_IDENTITY_PREFIX_LEN; i++) {
        if (!isupper((unsigned char)p[i])) return false;
    }
    return true;
}

static bool valid_hw_rev(char r)
{
    return r >= 'A' && r <= 'Z';
}

// SHA-256(mac)[:5 bytes] -> 40 bits -> 8 Crockford-32 chars (5 bits each, lossless).
// MAC alone is already globally unique by IEEE OUI assignment; hashing
// distributes those 48 bits evenly across the 40-bit output space so two
// adjacent MACs produce visually different IDs.
static void derive_unique(const uint8_t mac[6], char out[SK_IDENTITY_UNIQUE_LEN + 1])
{
    uint8_t hash[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, mac, 6);
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    uint64_t bits = ((uint64_t)hash[0] << 32) |
                    ((uint64_t)hash[1] << 24) |
                    ((uint64_t)hash[2] << 16) |
                    ((uint64_t)hash[3] <<  8) |
                    ((uint64_t)hash[4]);

    for (int i = SK_IDENTITY_UNIQUE_LEN - 1; i >= 0; i--) {
        out[i] = SK_ID_ALPHABET[bits & 0x1F];
        bits >>= 5;
    }
    out[SK_IDENTITY_UNIQUE_LEN] = '\0';
}

esp_err_t sk_identity_init(const char *prefix, char hw_rev)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (!valid_prefix(prefix)) {
        ESP_LOGE(TAG, "prefix must be 2 uppercase ASCII chars, got: %s", prefix ? prefix : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_hw_rev(hw_rev)) {
        ESP_LOGE(TAG, "hw_rev must be uppercase A-Z, got: 0x%02X", (unsigned)hw_rev);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_efuse_mac_get_default(s_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_efuse_mac_get_default failed: %s", esp_err_to_name(err));
        return err;
    }

    memcpy(s_prefix, prefix, SK_IDENTITY_PREFIX_LEN);
    s_prefix[SK_IDENTITY_PREFIX_LEN] = '\0';
    s_hw_rev = hw_rev;

    derive_unique(s_mac, s_unique);

    s_suffix[0] = s_hw_rev;
    memcpy(s_suffix + 1, s_unique, SK_IDENTITY_UNIQUE_LEN + 1);  // includes NUL

    snprintf(s_full, sizeof(s_full), "%s-%s", s_prefix, s_suffix);

    ESP_LOGI(TAG, "device id: %s (MAC %02X:%02X:%02X:%02X:%02X:%02X)",
             s_full, s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    s_initialized = true;
    return ESP_OK;
}

const char *sk_identity_get(void)        { return s_initialized ? s_full   : ""; }
const char *sk_identity_get_prefix(void) { return s_initialized ? s_prefix : ""; }
const char *sk_identity_get_suffix(void) { return s_initialized ? s_suffix : ""; }
const char *sk_identity_get_unique(void) { return s_initialized ? s_unique : ""; }
char        sk_identity_get_hw_rev(void) { return s_initialized ? s_hw_rev : '\0'; }

esp_err_t sk_identity_get_mac(uint8_t out[6])
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL)    return ESP_ERR_INVALID_ARG;
    memcpy(out, s_mac, 6);
    return ESP_OK;
}
