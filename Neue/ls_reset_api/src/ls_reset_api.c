// =====================================================================
// ls_reset_api — implementation. See header.
//
// esp_http_server üzerine sade GET /api/reset endpoint'i. Query'den
// `key=...` çıkarılır, NVS'teki api_key ile karşılaştırılır. Eşleşme
// varsa `timer.reset.requested` event'i `{"by":"api"}` ile yayınlanır;
// ls_timer_engine bu event'i dinleyip countdown'u sıfırlar.
//
// Server enabled/disabled toggle NVS'tedir. enabled=false ise port
// açılmaz; toggle değişiminde server start/stop edilir.
// =====================================================================

#include "ls_reset_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"
#include "sk_identity.h"

static const char *TAG = "ls_reset_api";

#define NVS_NS       "ls_rapi"
#define NVS_KEY_EN   "en"
#define NVS_KEY_KEY  "key"

#define HTTP_PORT    80
#define ENDPOINT     "/api/reset"

static bool       s_enabled = false;
static char       s_api_key[LS_RESET_API_KEY_LEN + 1];
static httpd_handle_t s_httpd = NULL;

// ---------------------------------------------------------------------
// Key generation
// ---------------------------------------------------------------------

static void generate_key(char *out, size_t cap)
{
    // Format: "ls_" + 12 lowercase hex chars + NUL
    static const char hex[] = "0123456789abcdef";
    uint8_t rnd[6];
    esp_fill_random(rnd, sizeof(rnd));
    out[0] = 'l'; out[1] = 's'; out[2] = '_';
    for (int i = 0; i < 6 && (size_t)(3 + i * 2 + 1) < cap; ++i) {
        out[3 + i * 2]     = hex[(rnd[i] >> 4) & 0xF];
        out[3 + i * 2 + 1] = hex[rnd[i] & 0xF];
    }
    out[15] = '\0';
    (void)cap;
}

// ---------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        // İlk boot: yeni key üret, disabled bırak
        generate_key(s_api_key, sizeof(s_api_key));
        s_enabled = false;
        return;
    }
    uint8_t u8;
    if (nvs_get_u8(h, NVS_KEY_EN, &u8) == ESP_OK) s_enabled = (u8 != 0);
    size_t len = sizeof(s_api_key);
    if (nvs_get_str(h, NVS_KEY_KEY, s_api_key, &len) != ESP_OK) {
        generate_key(s_api_key, sizeof(s_api_key));
    }
    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_EN, s_enabled ? 1 : 0);
    nvs_set_str(h, NVS_KEY_KEY, s_api_key);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------

static esp_err_t reset_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    // Defansif: toggle off iken handler tetiklenmemeli ama yarış halinde
    // (apply_enabled → server_stop devam ederken in-flight istek) gelirse 403.
    if (!s_enabled) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "{\"ok\":false,\"reason\":\"disabled\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    char query[64];
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > sizeof(query)) qlen = sizeof(query);
    if (qlen <= 1 || httpd_req_get_url_query_str(req, query, qlen) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"reason\":\"missing query\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    char key_param[LS_RESET_API_KEY_LEN + 1] = {0};
    if (httpd_query_key_value(query, "key", key_param, sizeof(key_param)) != ESP_OK
        || key_param[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"ok\":false,\"reason\":\"missing key\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    if (strcmp(key_param, s_api_key) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "{\"ok\":false,\"reason\":\"invalid key\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    // Eşleşti: reset request'i event bus'a yay
    sk_event_bus_publish("timer.reset.requested", "{\"by\":\"api\"}");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t s_uri_reset = {
    .uri      = ENDPOINT,
    .method   = HTTP_GET,
    .handler  = reset_get_handler,
    .user_ctx = NULL,
};

static esp_err_t server_start(void)
{
    if (s_httpd) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;
    httpd_register_uri_handler(s_httpd, &s_uri_reset);
    ESP_LOGI(TAG, "HTTP server up on :%d", HTTP_PORT);
    return ESP_OK;
}

static void server_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    ESP_LOGI(TAG, "HTTP server stopped");
}

static void apply_enabled(bool enabled)
{
    s_enabled = enabled;
    nvs_save();
    if (enabled) server_start();
    else         server_stop();
}

// ---------------------------------------------------------------------
// CLI helpers
// ---------------------------------------------------------------------

static bool key_is_valid(const char *s)
{
    // Expected: "ls_" + 12 lowercase hex chars, total 15 chars.
    if (!s) return false;
    if (strlen(s) != LS_RESET_API_KEY_LEN - 1) return false;
    if (s[0] != 'l' || s[1] != 's' || s[2] != '_') return false;
    for (int i = 3; i < LS_RESET_API_KEY_LEN - 1; ++i) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

// "********9089" formatinda maskeli key (key < 4 char ise "***").
static void mask_key(char *out, size_t cap)
{
    size_t L = strlen(s_api_key);
    if (L >= 4) {
        snprintf(out, cap, "********%s", s_api_key + L - 4);
    } else {
        snprintf(out, cap, "***");
    }
}

// "ls_xxxxxxxxx089" -> "089" (son 4 hane). Boyutu yetersizse "***".
static void last4(char *out, size_t cap)
{
    size_t L = strlen(s_api_key);
    if (L >= 4) {
        snprintf(out, cap, "%s", s_api_key + L - 4);
    } else {
        snprintf(out, cap, "***");
    }
}

// http://LS-XXXXXXXX.local/api/reset?key=ls_xxxxxxxxxxxx
// Full (unmasked) key is included so the URL is copy-pasteable into curl.
static void build_url(char *out, size_t cap)
{
    const char *id = sk_identity_get();
    snprintf(out, cap, "http://%s.local%s?key=%s",
             id ? id : "device", ENDPOINT, s_api_key);
}

// ---------------------------------------------------------------------
// CLI handlers
// ---------------------------------------------------------------------

static sk_err_t cli_enable(sk_cli_ctx_t *ctx)
{
    const char *v = sk_cli_arg_after(ctx, "value");
    if (!v) v = sk_cli_arg(ctx, 0);
    if (!v) {
        sk_cli_usage(ctx,
            "reset_api enable <on|off>",
            "on  | yes | true  | 1  -> enabled\n"
            "off | no  | false | 0  -> disabled",
            "reset_api enable on\n"
            "reset_api enable off");
        return SK_ERR_MISSING_ARG;
    }
    bool enabled;
    if (strcmp(v, "on") == 0 || strcmp(v, "yes") == 0 ||
        strcmp(v, "true") == 0 || strcmp(v, "1") == 0) {
        enabled = true;
    } else if (strcmp(v, "off") == 0 || strcmp(v, "no") == 0 ||
               strcmp(v, "false") == 0 || strcmp(v, "0") == 0) {
        enabled = false;
    } else {
        sk_cli_usage(ctx,
            "reset_api enable <on|off>",
            "on  | yes | true  | 1  -> enabled\n"
            "off | no  | false | 0  -> disabled",
            "reset_api enable on");
        return SK_ERR_INVALID_ARG;
    }

    apply_enabled(enabled);

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"enabled\":%s}", enabled ? "true" : "false");

    if (sk_cli_is_machine_mode(ctx)) {
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }
    sk_cli_kv(ctx, "Status", enabled ? "enabled" : "disabled");
    return SK_OK;
}

static sk_err_t cli_set_key(sk_cli_ctx_t *ctx)
{
    const char *k = sk_cli_arg_after(ctx, "key");
    if (!k) k = sk_cli_arg(ctx, 0);
    if (!k) {
        sk_cli_usage(ctx,
            "reset_api key <ls_xxxxxxxxxxxx>",
            "key: 'ls_' + 12 lowercase hex (15 chars)",
            "reset_api key ls_9a3b1c4f7e02");
        return SK_ERR_MISSING_ARG;
    }
    if (!key_is_valid(k)) {
        sk_cli_usage(ctx,
            "reset_api key <ls_xxxxxxxxxxxx>",
            "key: 'ls_' + 12 lowercase hex (15 chars)\n"
            "lowercase hex: 0-9, a-f",
            "reset_api key ls_9a3b1c4f7e02");
        return SK_ERR_INVALID_ARG;
    }

    strncpy(s_api_key, k, sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';
    nvs_save();

    char masked[24];
    mask_key(masked, sizeof(masked));

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"key\":\"%s\"}", masked);

    if (sk_cli_is_machine_mode(ctx)) {
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }
    sk_cli_kv(ctx, "New key", masked);
    return SK_OK;
}

static sk_err_t cli_regen(sk_cli_ctx_t *ctx)
{
    generate_key(s_api_key, sizeof(s_api_key));
    nvs_save();

    char tail[8];
    last4(tail, sizeof(tail));

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"last4\":\"%s\"}", tail);

    if (sk_cli_is_machine_mode(ctx)) {
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }
    sk_cli_write(ctx, "New key generated\n", 18);
    sk_cli_kv(ctx, "  last 4 chars", tail);
    return SK_OK;
}

static sk_err_t cli_get(sk_cli_ctx_t *ctx)
{
    char masked[24];
    mask_key(masked, sizeof(masked));
    char url[96];
    build_url(url, sizeof(url));

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"key\":\"%s\",\"port\":%d,\"endpoint\":\"%s\",\"url\":\"%s\"}",
        s_enabled ? "true" : "false", masked, HTTP_PORT, ENDPOINT, url);

    if (sk_cli_is_machine_mode(ctx)) {
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }
    sk_cli_kv (ctx, "Enabled",  s_enabled ? "yes" : "no");
    sk_cli_kv (ctx, "Key",      masked);
    sk_cli_kvf(ctx, "Port",     "%d", HTTP_PORT);
    sk_cli_kv (ctx, "Endpoint", ENDPOINT);
    sk_cli_kv (ctx, "URL",      url);
    return SK_OK;
}

static sk_err_t cli_status(sk_cli_ctx_t *ctx)
{
    bool running = (s_httpd != NULL);
    char buf[96];
    snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"running\":%s,\"port\":%d}",
        s_enabled ? "true" : "false",
        running ? "true" : "false", HTTP_PORT);

    if (sk_cli_is_machine_mode(ctx)) {
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }
    sk_cli_kv (ctx, "Enabled",       s_enabled ? "yes" : "no");
    sk_cli_kv (ctx, "Server status", running ? "running" : "stopped");
    sk_cli_kvf(ctx, "Port",          "%d", HTTP_PORT);
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name    = "reset_api.enable",
      .summary = "Enable/disable remote reset HTTP server: reset_api enable <on|off>",
      .usage   = "reset_api enable <on|off>",
      .help_block =
          "Enable or disable the inbound HTTP reset endpoint.\n"
          "\n"
          "  state: on | off (also yes/no/true/false/1/0)\n"
          "\n"
          "When enabled the device listens on port 80 at /api/reset and\n"
          "accepts HTTPS-less GET requests with a valid `key=` query\n"
          "parameter. Each valid request resets the running countdown\n"
          "(equivalent to pressing the physical reset button).\n"
          "\n"
          "SECURITY: this endpoint is plain HTTP on the LAN. The key is\n"
          "the only access control. Use `reset_api regen` periodically.\n"
          "Use SKAPP's BLE/ECDH+HMAC channel for stronger guarantees.\n"
          "\n"
          "Examples:\n"
          "  reset_api enable on\n"
          "  reset_api enable off",
      .handler = cli_enable },
    { .name    = "reset_api.key",
      .summary = "Set the API key (ls_ + 12 lowercase hex)",
      .usage   = "reset_api key <ls_xxxxxxxxxxxx>",
      .help_block =
          "Set the API key manually (format-validated).\n"
          "\n"
          "  value: must be exactly \"ls_\" + 12 lowercase hex chars\n"
          "         (15 chars total). Use `reset_api regen` to get a\n"
          "         fresh random key in the correct format.\n"
          "\n"
          "This is rarely needed; the device generates a key on first\n"
          "boot and again on `reset_api regen`. Use this only to share\n"
          "a fixed key between multiple devices in a script.\n"
          "\n"
          "Examples:\n"
          "  reset_api key ls_a1b2c3d4e5f6\n"
          "  reset_api regen          # preferred way",
      .handler = cli_set_key },
    { .name    = "reset_api.regen",
      .summary = "Generate a fresh random API key",
      .usage   = "reset_api regen",
      .help_block =
          "Generate a fresh random API key.\n"
          "\n"
          "No arguments. The previous key is invalidated immediately;\n"
          "update your callers (curl scripts, automation) before relying\n"
          "on the new key.\n"
          "\n"
          "The key format is \"ls_\" + 12 lowercase hex chars (48 bits of\n"
          "entropy). After regen, run `reset_api get` to copy the full\n"
          "URL.\n"
          "\n"
          "Examples:\n"
          "  reset_api regen",
      .handler = cli_regen },
    { .name    = "reset_api.get",
      .summary = "Show remote reset API config (key partially masked)",
      .usage   = "reset_api get",
      .help_block =
          "Show the reset API configuration plus a copyable URL.\n"
          "\n"
          "No arguments. The API key is partially masked in human output\n"
          "(only the last 4 chars are visible); the full URL is printed\n"
          "including the unmasked key so it can be copied into a script.\n"
          "\n"
          "Use the printed URL with curl:\n"
          "  curl http://LS-XXXXXXXX.local/api/reset?key=ls_...\n"
          "\n"
          "Examples:\n"
          "  reset_api get",
      .handler = cli_get },
    { .name    = "reset_api.status",
      .summary = "Show HTTP server runtime status",
      .usage   = "reset_api status",
      .help_block =
          "Show the HTTP server runtime status.\n"
          "\n"
          "Reports whether the API is enabled (configured to run) and\n"
          "whether the server socket is currently bound. Both should\n"
          "be true after `reset_api enable on`.\n"
          "\n"
          "Examples:\n"
          "  reset_api status",
      .handler = cli_status },
};

// ---------------------------------------------------------------------
// Factory reset hook
// ---------------------------------------------------------------------

// On device.factory-reset.requested: STOP the HTTP server FIRST so the
// inbound endpoint with the old shared key stops answering, then wipe
// NVS + regenerate a fresh random key + persist it so subsequent boots
// have a valid key. Pairing-completeness fix: previously the api_key
// (effectively a shared secret) and enabled flag survived factory
// reset, leaving the HTTP endpoint open to the previous owner.
static void on_factory_reset(const sk_event_t *evt, void *user_ctx)
{
    (void)evt; (void)user_ctx;
    ESP_LOGW(TAG, "factory reset received — stopping HTTP server + wiping ls_reset_api");

    // 1) CRITICAL: stop the HTTP server before anything else so the old
    // key can never answer another request, even racing with handler
    // dispatch.
    if (s_enabled) {
        server_stop();
    }
    s_enabled = false;

    // 2) Regenerate a fresh random api_key.
    generate_key(s_api_key, sizeof(s_api_key));

    // 3) Wipe NVS namespace, then persist the freshly generated key so
    // subsequent boots find a valid key (matches nvs_load's first-boot
    // expectation).
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    // nvs_save() writes both en and key in one namespace; safe to call
    // immediately after erase_all because nvs_open re-creates the ns.
    nvs_save();
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

bool ls_reset_api_is_enabled(void)
{
    return s_enabled;
}

esp_err_t ls_reset_api_init(void)
{
    nvs_load();
    if (s_enabled) {
        esp_err_t err = server_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "server_start failed: %s; api remains marked enabled",
                     esp_err_to_name(err));
        }
    }
    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); ++i) {
        sk_cli_register(&s_cmds[i]);
    }

    // Factory reset hook — stop HTTP, regenerate key, wipe NVS.
    int sub;
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, &sub);

    ESP_LOGI(TAG, "init: enabled=%d key=...%s",
             (int)s_enabled,
             (strlen(s_api_key) >= 4) ? (s_api_key + strlen(s_api_key) - 4) : "***");
    return ESP_OK;
}
