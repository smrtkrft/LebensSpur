#include "sk_auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"

#define CONFIRM_SLOTS 4

typedef struct {
    char    token_hex[SK_AUTH_CONFIRM_TOKEN_LEN + 1];
    int64_t expires_us;
    bool    in_use;
} confirm_slot_t;

static confirm_slot_t s_slots[CONFIRM_SLOTS];

static void to_hex(const uint8_t *bin, size_t n, char *out)
{
    static const char *HEX = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i + 0] = HEX[(bin[i] >> 4) & 0xF];
        out[2 * i + 1] = HEX[bin[i] & 0xF];
    }
    out[2 * n] = '\0';
}

esp_err_t sk_auth_confirm_issue(char out_hex[SK_AUTH_CONFIRM_TOKEN_LEN + 1],
                                uint32_t *out_ttl_sec)
{
    if (!out_hex) return ESP_ERR_INVALID_ARG;
    uint8_t raw[SK_AUTH_CONFIRM_TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));

    int64_t now = esp_timer_get_time();
    // Find a free slot; else overwrite the oldest.
    int slot = -1;
    int64_t oldest = INT64_MAX;
    int oldest_idx = 0;
    for (int i = 0; i < CONFIRM_SLOTS; i++) {
        if (!s_slots[i].in_use || s_slots[i].expires_us < now) { slot = i; break; }
        if (s_slots[i].expires_us < oldest) { oldest = s_slots[i].expires_us; oldest_idx = i; }
    }
    if (slot < 0) slot = oldest_idx;

    to_hex(raw, sizeof(raw), s_slots[slot].token_hex);
    s_slots[slot].expires_us = now + (int64_t)SK_AUTH_CONFIRM_TTL_SEC * 1000000LL;
    s_slots[slot].in_use     = true;

    strcpy(out_hex, s_slots[slot].token_hex);
    if (out_ttl_sec) *out_ttl_sec = SK_AUTH_CONFIRM_TTL_SEC;
    return ESP_OK;
}

esp_err_t sk_auth_confirm_consume(const char *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < CONFIRM_SLOTS; i++) {
        if (!s_slots[i].in_use) continue;
        if (s_slots[i].expires_us < now) { s_slots[i].in_use = false; continue; }
        if (strcmp(s_slots[i].token_hex, token) == 0) {
            s_slots[i].in_use = false;  // single use
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
