#include "sk_passphrase.h"

#include "sk_event_bus.h"
#include "sk_capabilities.h"
#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_auth.h"   // confirm_consume
#include "sk_log.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "sk_pass";

#define NVS_NS              "sk_pass"
#define NVS_KEY_SALT        "salt"        // 16B blob
#define NVS_KEY_HASH        "hash"        // 32B blob
#define NVS_KEY_MODE        "mode"        // 1B u8 bitmask (bit0=pairing, bit1=always)
#define NVS_KEY_FAIL_COUNT  "fail_n"      // 1B u8

#define MODE_BIT_PAIRING    (1u << 0)
#define MODE_BIT_ALWAYS     (1u << 1)

static SemaphoreHandle_t s_mtx           = NULL;
static bool              s_has_pass      = false;
static uint8_t           s_salt[SK_PASSPHRASE_SALT_LEN];
static uint8_t           s_hash[SK_PASSPHRASE_HASH_LEN];
static uint8_t           s_mode_bits     = 0;
static uint8_t           s_fail_count    = 0;

// -- NVS helpers -----------------------------------------------------------

static esp_err_t nvs_save_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

static void nvs_load_state(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t sz = sizeof(s_salt);
    bool have_salt = (nvs_get_blob(h, NVS_KEY_SALT, s_salt, &sz) == ESP_OK
                      && sz == SK_PASSPHRASE_SALT_LEN);
    sz = sizeof(s_hash);
    bool have_hash = (nvs_get_blob(h, NVS_KEY_HASH, s_hash, &sz) == ESP_OK
                      && sz == SK_PASSPHRASE_HASH_LEN);
    s_has_pass = have_salt && have_hash;

    uint8_t mode = 0;
    if (nvs_get_u8(h, NVS_KEY_MODE, &mode) == ESP_OK) s_mode_bits = mode;

    uint8_t fc = 0;
    if (nvs_get_u8(h, NVS_KEY_FAIL_COUNT, &fc) == ESP_OK) s_fail_count = fc;

    nvs_close(h);
}

static esp_err_t nvs_wipe(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

// -- KDF -------------------------------------------------------------------

// Custom PBKDF2-SHA256 with periodic vTaskDelay so IDLE0 (and the task
// watchdog backing it) actually gets CPU. Output is fixed at 32 bytes,
// so PBKDF2 reduces to a single block:
//   U_1 = HMAC(P, salt || INT(1))
//   U_i = HMAC(P, U_{i-1})
//   T_1 = U_1 XOR U_2 XOR ... XOR U_iters
//
// We give up CPU every 128 iterations via vTaskDelay(1) — taskYIELD
// alone isn't enough on single-core ESP32-C6 because the CLI task and
// IDLE share the same priority class; only an actual sleep lets the WDT
// see IDLE. Iteration count is intentionally low (see header for the
// security rationale).
static esp_err_t pbkdf2(const char *plain,
                        const uint8_t salt[SK_PASSPHRASE_SALT_LEN],
                        uint8_t out[SK_PASSPHRASE_HASH_LEN])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return ESP_FAIL;

    mbedtls_md_context_t hmac;
    mbedtls_md_init(&hmac);
    int rc = mbedtls_md_setup(&hmac, md, /*hmac=*/1);
    if (rc != 0) {
        mbedtls_md_free(&hmac);
        ESP_LOGE(TAG, "md_setup: -0x%04X", -rc);
        return ESP_FAIL;
    }

    const size_t plen = strlen(plain);
    const size_t hlen = SK_PASSPHRASE_HASH_LEN;  // 32

    uint8_t U[32];
    uint8_t T[32];

    // U_1 = HMAC(P, salt || INT(1))
    static const uint8_t int1[4] = {0, 0, 0, 1};
    rc  = mbedtls_md_hmac_starts(&hmac, (const uint8_t *)plain, plen);
    if (rc == 0) rc = mbedtls_md_hmac_update(&hmac, salt, SK_PASSPHRASE_SALT_LEN);
    if (rc == 0) rc = mbedtls_md_hmac_update(&hmac, int1, sizeof(int1));
    if (rc == 0) rc = mbedtls_md_hmac_finish(&hmac, U);
    if (rc != 0) goto fail;
    memcpy(T, U, hlen);

    // U_i = HMAC(P, U_{i-1}); fold into T via XOR.
    // Yield every 128 iters so IDLE/WDT keep up.
    for (uint32_t i = 1; i < SK_PASSPHRASE_PBKDF2_ITERS; i++) {
        rc = mbedtls_md_hmac_reset(&hmac);
        if (rc == 0) rc = mbedtls_md_hmac_update(&hmac, U, hlen);
        if (rc == 0) rc = mbedtls_md_hmac_finish(&hmac, U);
        if (rc != 0) goto fail;
        for (size_t j = 0; j < hlen; j++) T[j] ^= U[j];

        if ((i & 0x7F) == 0) {
            // Raw 1-tick sleep — relinquishes CPU to IDLE so the task
            // watchdog stays fed even though our task isn't subscribed.
            // Do NOT use pdMS_TO_TICKS(1): at FREERTOS_HZ=100 it is 0.
            vTaskDelay(1);
        }
    }

    memcpy(out, T, hlen);
    mbedtls_md_free(&hmac);
    return ESP_OK;

fail:
    mbedtls_md_free(&hmac);
    ESP_LOGE(TAG, "pbkdf2 hmac error: -0x%04X", -rc);
    return ESP_FAIL;
}

static bool valid_plain_len(const char *plain)
{
    if (!plain) return false;
    size_t n = strlen(plain);
    return n >= SK_PASSPHRASE_MIN_LEN && n <= SK_PASSPHRASE_MAX_LEN;
}

static bool ct_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

// -- Public API ------------------------------------------------------------

bool sk_passphrase_is_set(void) { return s_has_pass; }

sk_pass_mode_t sk_passphrase_get_mode(void)
{
    sk_pass_mode_t m = {0};
    if (!s_has_pass) return m;
    m.pairing_required = (s_mode_bits & MODE_BIT_PAIRING) != 0;
    m.always_required  = (s_mode_bits & MODE_BIT_ALWAYS)  != 0;
    return m;
}

esp_err_t sk_passphrase_set_mode(bool pairing_required, bool always_required)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_has_pass) {
        // Toggles are meaningless without a passphrase; force-clear and
        // refuse the write so callers don't leave stale bits behind.
        s_mode_bits = 0;
        nvs_save_u8(NVS_KEY_MODE, 0);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bits = 0;
    if (pairing_required) bits |= MODE_BIT_PAIRING;
    if (always_required)  bits |= MODE_BIT_ALWAYS;
    s_mode_bits = bits;
    esp_err_t err = nvs_save_u8(NVS_KEY_MODE, bits);
    xSemaphoreGive(s_mtx);
    if (err == ESP_OK) {
        char p[64];
        snprintf(p, sizeof(p), "{\"pairing\":%s,\"always\":%s}",
                 pairing_required ? "true" : "false",
                 always_required  ? "true" : "false");
        sk_event_bus_publish("auth.passphrase.mode.changed", p);
    }
    return err;
}

esp_err_t sk_passphrase_set(const char *plain)
{
    if (!valid_plain_len(plain)) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_has_pass) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;  // use change(old, new)
    }

    uint8_t salt[SK_PASSPHRASE_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));

    uint8_t hash[SK_PASSPHRASE_HASH_LEN];
    esp_err_t err = pbkdf2(plain, salt, hash);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mtx);
        return err;
    }

    err = nvs_save_blob(NVS_KEY_SALT, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_save_blob(NVS_KEY_HASH, hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_save_u8(NVS_KEY_FAIL_COUNT, 0);

    if (err == ESP_OK) {
        memcpy(s_salt, salt, sizeof(s_salt));
        memcpy(s_hash, hash, sizeof(s_hash));
        s_fail_count = 0;
        s_has_pass   = true;
    }
    // Wipe stack copies regardless.
    memset(salt, 0, sizeof(salt));
    memset(hash, 0, sizeof(hash));

    xSemaphoreGive(s_mtx);
    if (err == ESP_OK) {
        sk_event_bus_publish("auth.passphrase.set", NULL);
        ESP_LOGI(TAG, "passphrase set");
    }
    return err;
}

// Verify and *do not* mutate the fail counter on the success path.
// On mismatch, increment + persist. Returns ESP_OK / INVALID_RESPONSE /
// INVALID_STATE / FAIL.
static esp_err_t verify_internal(const char *plain, uint8_t *out_left,
                                 bool reset_on_match)
{
    if (!plain) return ESP_ERR_INVALID_ARG;
    if (!s_has_pass) return ESP_ERR_INVALID_STATE;

    uint8_t cand[SK_PASSPHRASE_HASH_LEN];
    esp_err_t err = pbkdf2(plain, s_salt, cand);
    if (err != ESP_OK) return err;

    bool match = ct_eq(cand, s_hash, SK_PASSPHRASE_HASH_LEN);
    memset(cand, 0, sizeof(cand));

    if (match) {
        if (reset_on_match && s_fail_count != 0) {
            s_fail_count = 0;
            nvs_save_u8(NVS_KEY_FAIL_COUNT, 0);
        }
        if (out_left) {
            uint8_t remain = (s_fail_count >= SK_PASSPHRASE_FAIL_LOCKOUT)
                                 ? 0
                                 : (uint8_t)(SK_PASSPHRASE_FAIL_LOCKOUT - s_fail_count);
            *out_left = remain;
        }
        return ESP_OK;
    }

    // Mismatch: bump persistent counter.
    if (s_fail_count < 0xFF) s_fail_count++;
    nvs_save_u8(NVS_KEY_FAIL_COUNT, s_fail_count);

    uint8_t remain = (s_fail_count >= SK_PASSPHRASE_FAIL_LOCKOUT)
                         ? 0
                         : (uint8_t)(SK_PASSPHRASE_FAIL_LOCKOUT - s_fail_count);
    if (out_left) *out_left = remain;

    if (s_fail_count >= SK_PASSPHRASE_FAIL_LOCKOUT) {
        ESP_LOGW(TAG, "passphrase fail count reached lockout (%u) — triggering factory reset",
                 (unsigned)s_fail_count);
        SK_LOG_E("auth", "passphrase.lockout", "triggered=factory-reset");
        // Publish synchronously: every subscriber (sk_auth, bf_secure_store,
        // sk_passphrase, sk_api, sk_wifi) wipes its NVS state inline.
        sk_event_bus_publish("device.factory-reset.requested",
                             "{\"reason\":\"passphrase_lockout\"}");
        // Brief delay so any deferred NVS commits flush before the reset.
        // Matches the pattern in sk_control.c after a long-press / multi-tap.
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        // Unreachable.
    } else {
        char p[64];
        snprintf(p, sizeof(p), "{\"attempts_left\":%u}", (unsigned)remain);
        sk_event_bus_publish("auth.passphrase.failed", p);
        SK_LOG_W("auth", "passphrase.fail", "attempts_left=%u", (unsigned)remain);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t sk_passphrase_verify(const char *plain, uint8_t *out_attempts_left)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = verify_internal(plain, out_attempts_left, /*reset=*/true);
    xSemaphoreGive(s_mtx);
    return err;
}

esp_err_t sk_passphrase_change(const char *old_plain, const char *new_plain)
{
    if (!valid_plain_len(new_plain)) return ESP_ERR_INVALID_ARG;
    if (!old_plain) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_has_pass) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = verify_internal(old_plain, NULL, /*reset=*/true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mtx);
        return err;  // INVALID_RESPONSE → caller surfaces attempts_left next call
    }

    uint8_t salt[SK_PASSPHRASE_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    uint8_t hash[SK_PASSPHRASE_HASH_LEN];
    err = pbkdf2(new_plain, salt, hash);
    if (err == ESP_OK) err = nvs_save_blob(NVS_KEY_SALT, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_save_blob(NVS_KEY_HASH, hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_save_u8(NVS_KEY_FAIL_COUNT, 0);

    if (err == ESP_OK) {
        memcpy(s_salt, salt, sizeof(s_salt));
        memcpy(s_hash, hash, sizeof(s_hash));
        s_fail_count = 0;
    }
    memset(salt, 0, sizeof(salt));
    memset(hash, 0, sizeof(hash));

    xSemaphoreGive(s_mtx);
    if (err == ESP_OK) {
        sk_event_bus_publish("auth.passphrase.changed", NULL);
        ESP_LOGI(TAG, "passphrase changed");
    }
    return err;
}

esp_err_t sk_passphrase_clear(const char *old_plain)
{
    if (!old_plain) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_has_pass) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = verify_internal(old_plain, NULL, /*reset=*/true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mtx);
        return err;
    }

    nvs_wipe();
    memset(s_salt, 0, sizeof(s_salt));
    memset(s_hash, 0, sizeof(s_hash));
    s_mode_bits  = 0;
    s_fail_count = 0;
    s_has_pass   = false;
    xSemaphoreGive(s_mtx);

    sk_event_bus_publish("auth.passphrase.cleared", NULL);
    ESP_LOGI(TAG, "passphrase cleared");
    return ESP_OK;
}

uint8_t sk_passphrase_fail_count(void) { return s_fail_count; }

uint8_t sk_passphrase_attempts_left(void)
{
    if (!s_has_pass) return SK_PASSPHRASE_FAIL_LOCKOUT;
    if (s_fail_count >= SK_PASSPHRASE_FAIL_LOCKOUT) return 0;
    return (uint8_t)(SK_PASSPHRASE_FAIL_LOCKOUT - s_fail_count);
}

// -- CLI commands -----------------------------------------------------------

static const char *get_arg_str(sk_cli_ctx_t *ctx, const char *key, int human_idx)
{
    if (sk_cli_is_machine_mode(ctx)) {
        return sk_cli_arg_named(ctx, key);
    }
    return sk_cli_arg(ctx, human_idx);
}

// Returns true if a confirm-token error was emitted (caller should bail).
// Returns false on success — caller proceeds with the actual command.
static bool reject_without_confirm(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return true; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return true;
    }
    return false;
}

static sk_err_t cmd_pass_status(sk_cli_ctx_t *ctx)
{
    sk_pass_mode_t m = sk_passphrase_get_mode();
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"set\":%s,\"mode\":{\"pairing\":%s,\"always\":%s},"
             "\"attempts_left\":%u,\"min_len\":%u,\"max_len\":%u}",
             s_has_pass ? "true" : "false",
             m.pairing_required ? "true" : "false",
             m.always_required ? "true" : "false",
             (unsigned)sk_passphrase_attempts_left(),
             (unsigned)SK_PASSPHRASE_MIN_LEN,
             (unsigned)SK_PASSPHRASE_MAX_LEN);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static sk_err_t cmd_pass_set(sk_cli_ctx_t *ctx)
{
    if (reject_without_confirm(ctx)) return SK_OK;
    const char *plain = get_arg_str(ctx, "plain", 0);
    if (!plain) { sk_cli_err(ctx, SK_ERR_INVALID_ARG, NULL); return SK_OK; }
    esp_err_t err = sk_passphrase_set(plain);
    if (err == ESP_ERR_INVALID_ARG)   { sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"reason\":\"length\"}"); return SK_OK; }
    if (err == ESP_ERR_INVALID_STATE) { sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"already_set\"}");   return SK_OK; }
    if (err != ESP_OK)                { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK; }
    sk_cli_ok(ctx, "{\"set\":true}");
    return SK_OK;
}

static sk_err_t cmd_pass_change(sk_cli_ctx_t *ctx)
{
    if (reject_without_confirm(ctx)) return SK_OK;
    const char *oldp = get_arg_str(ctx, "old", 0);
    const char *newp = get_arg_str(ctx, "new", 1);
    if (!oldp || !newp) { sk_cli_err(ctx, SK_ERR_INVALID_ARG, NULL); return SK_OK; }
    esp_err_t err = sk_passphrase_change(oldp, newp);
    if (err == ESP_ERR_INVALID_ARG)      { sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"reason\":\"length\"}"); return SK_OK; }
    if (err == ESP_ERR_INVALID_STATE)    { sk_cli_err(ctx, SK_ERR_BUSY,        "{\"reason\":\"not_set\"}"); return SK_OK; }
    if (err == ESP_ERR_INVALID_RESPONSE) {
        char p[80];
        snprintf(p, sizeof(p), "{\"reason\":\"wrong_passphrase\",\"attempts_left\":%u}",
                 (unsigned)sk_passphrase_attempts_left());
        sk_cli_err(ctx, SK_ERR_INVALID_ARG, p);
        return SK_OK;
    }
    if (err != ESP_OK) { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK; }
    sk_cli_ok(ctx, "{\"changed\":true}");
    return SK_OK;
}

static sk_err_t cmd_pass_clear(sk_cli_ctx_t *ctx)
{
    if (reject_without_confirm(ctx)) return SK_OK;
    const char *oldp = get_arg_str(ctx, "old", 0);
    if (!oldp) { sk_cli_err(ctx, SK_ERR_INVALID_ARG, NULL); return SK_OK; }
    esp_err_t err = sk_passphrase_clear(oldp);
    if (err == ESP_ERR_INVALID_STATE)    { sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"not_set\"}"); return SK_OK; }
    if (err == ESP_ERR_INVALID_RESPONSE) {
        char p[80];
        snprintf(p, sizeof(p), "{\"reason\":\"wrong_passphrase\",\"attempts_left\":%u}",
                 (unsigned)sk_passphrase_attempts_left());
        sk_cli_err(ctx, SK_ERR_INVALID_ARG, p);
        return SK_OK;
    }
    if (err != ESP_OK) { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK; }
    sk_cli_ok(ctx, "{\"cleared\":true}");
    return SK_OK;
}

static sk_err_t cmd_pass_mode_set(sk_cli_ctx_t *ctx)
{
    if (reject_without_confirm(ctx)) return SK_OK;
    long pairing = 0, always = 0;
    bool have_p = sk_cli_arg_long(ctx, "pairing", &pairing);
    bool have_a = sk_cli_arg_long(ctx, "always",  &always);
    if (!have_p || !have_a) { sk_cli_err(ctx, SK_ERR_INVALID_ARG, NULL); return SK_OK; }
    esp_err_t err = sk_passphrase_set_mode(pairing != 0, always != 0);
    if (err == ESP_ERR_INVALID_STATE) { sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"not_set\"}"); return SK_OK; }
    if (err != ESP_OK) { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK; }
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"pairing\":%s,\"always\":%s}",
             pairing ? "true" : "false",
             always  ? "true" : "false");
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static const sk_cli_command_t s_pass_cmds[] = {
    { .name = "auth.passphrase.status",
      .summary = "Show passphrase configuration (set / mode / attempts left)",
      .usage   = "auth passphrase status",
      .help_block =
          "Reports whether a content-access passphrase is configured, which\n"
          "of the two enforcement modes are active, and how many wrong\n"
          "attempts remain before the device factory-resets.\n",
      .handler = cmd_pass_status },

    { .name = "auth.passphrase.set",
      .summary = "Set the content-access passphrase (first time)",
      .usage   = "auth passphrase set <plain>",
      .critical = true,
      .help_block =
          "Sets a new passphrase when none exists. Length: 6-32 characters.\n"
          "Use `auth passphrase change` to rotate; `auth passphrase clear` to\n"
          "remove. Critical — needs a confirm token.\n",
      .handler = cmd_pass_set },

    { .name = "auth.passphrase.change",
      .summary = "Rotate the passphrase (verifies old, sets new)",
      .usage   = "auth passphrase change <old> <new>",
      .critical = true,
      .handler = cmd_pass_change },

    { .name = "auth.passphrase.clear",
      .summary = "Remove the passphrase (verifies old)",
      .usage   = "auth passphrase clear <old>",
      .critical = true,
      .handler = cmd_pass_clear },

    { .name = "auth.passphrase.mode.set",
      .summary = "Set the two enforcement toggles",
      .usage   = "auth passphrase mode set --pairing <0|1> --always <0|1>",
      .critical = true,
      .help_block =
          "Two independent toggles:\n"
          "  pairing  — ask for the passphrase only during the first-time\n"
          "             ECDH pairing of a new SKAPP install.\n"
          "  always   — ask on every connection. Subsumes 'pairing'.\n"
          "Both off = passphrase exists but isn't enforced.\n",
      .handler = cmd_pass_mode_set },
};

// -- Init -------------------------------------------------------------------

static void on_factory_reset_requested(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    nvs_wipe();
    memset(s_salt, 0, sizeof(s_salt));
    memset(s_hash, 0, sizeof(s_hash));
    s_mode_bits  = 0;
    s_fail_count = 0;
    s_has_pass   = false;
    xSemaphoreGive(s_mtx);
    ESP_LOGW(TAG, "passphrase wiped (factory reset)");
}

esp_err_t sk_passphrase_init(void)
{
    if (s_mtx) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;

    nvs_load_state();

    for (size_t i = 0; i < sizeof(s_pass_cmds)/sizeof(s_pass_cmds[0]); i++) {
        sk_cli_register(&s_pass_cmds[i]);
    }

    int sub;
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset_requested, NULL, &sub);
    sk_capabilities_register_book("sk_passphrase", "0.1.0");

    ESP_LOGI(TAG, "sk_passphrase ready (set=%d, mode=%s%s, fail=%u)",
             (int)s_has_pass,
             (s_mode_bits & MODE_BIT_PAIRING) ? "P" : "-",
             (s_mode_bits & MODE_BIT_ALWAYS)  ? "A" : "-",
             (unsigned)s_fail_count);
    return ESP_OK;
}
