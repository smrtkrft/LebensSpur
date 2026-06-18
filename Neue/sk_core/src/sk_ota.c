// Two-step OTA. Manifest fetched + parsed (cJSON) on check; URL + SHA256
// cached internally. Download uses esp_https_ota in step-by-step mode so
// SHA256 can be verified against the written partition before the boot
// partition is finalised — a hash mismatch aborts the flow without ever
// activating the bad image.

#include "sk_ota.h"
#include "sk_auth.h"
#include "sk_capabilities.h"
#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"
#include "sk_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "sk_ota";

#define MANIFEST_BUF_SIZE   1536      // manifest.json max size
#define HASH_READ_CHUNK     1024
#define DOWNLOAD_STACK      8192

// GitHub release downloads 302-redirect to release-assets.githubusercontent.com
// with ~900-char signed (JWT) URLs. The default 512-byte esp_http_client
// buffers cannot serialise a GET request line that long: open() then fails in
// http_client_prepare_first_line() ("Out of buffer"), which surfaces to the
// user as "http open". Size the TX (request) and RX (response-header) buffers
// to hold the long URL plus headers comfortably.
#define OTA_HTTP_BUF_SIZE   4096

static sk_ota_cfg_t      s_cfg     = {0};
static sk_ota_status_t   s_status  = { .state = SK_OTA_IDLE };
static SemaphoreHandle_t s_mtx     = NULL;
static char              s_cfg_manifest_url[192] = {0};   // owned copy of cfg->manifest_url
static char              s_cfg_fw_version[16]    = {0};   // owned copy of cfg->fw_version

// -- State helpers ----------------------------------------------------------

static void set_state(sk_ota_state_t st, const char *msg)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_status.state = st;
    if (msg) {
        strncpy(s_status.message, msg, sizeof(s_status.message) - 1);
        s_status.message[sizeof(s_status.message) - 1] = '\0';
    }
    xSemaphoreGive(s_mtx);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"progress\":%d,\"current\":\"%s\","
             "\"remote\":\"%s\",\"message\":\"%s\"}",
             sk_ota_state_str(st), s_status.progress_pct,
             s_status.current_version, s_status.remote_version,
             s_status.message);
    sk_event_bus_publish("ota.fw.state", payload);
}

static void set_progress(int pct)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_status.progress_pct = pct;
    xSemaphoreGive(s_mtx);
}

bool sk_ota_is_busy(void)
{
    return s_status.state == SK_OTA_CHECKING || s_status.state == SK_OTA_DOWNLOADING;
}

bool sk_ota_can_rollback(void)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (!other) return false;
    esp_app_desc_t desc;
    return esp_ota_get_partition_description(other, &desc) == ESP_OK;
}

const char *sk_ota_running_label(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "";
}

const char *sk_ota_state_str(sk_ota_state_t s)
{
    switch (s) {
    case SK_OTA_IDLE:             return "idle";
    case SK_OTA_CHECKING:         return "checking";
    case SK_OTA_UPDATE_AVAILABLE: return "update_available";
    case SK_OTA_NO_UPDATE:        return "no_update";
    case SK_OTA_DOWNLOADING:      return "downloading";
    case SK_OTA_DONE:             return "done";
    case SK_OTA_ERROR:            return "error";
    default:                      return "?";
    }
}

void sk_ota_get_status(sk_ota_status_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mtx);
}

// -- Semver compare --------------------------------------------------------
//
// Returns >0 if a > b, <0 if a < b, 0 if equal. Handles "X", "X.Y", "X.Y.Z";
// missing components count as 0. Pre-release tags (e.g. "1.0.0-beta") are
// ignored beyond the dotted prefix — they compare equal to the release
// number, which is good enough for the SmartKraft fleet.

static int parse_semver(const char *s, int *maj, int *min, int *pat)
{
    *maj = *min = *pat = 0;
    if (!s) return 0;
    return sscanf(s, "%d.%d.%d", maj, min, pat);
}

static int semver_cmp(const char *a, const char *b)
{
    int am, an, ap;
    int bm, bn, bp;
    parse_semver(a, &am, &an, &ap);
    parse_semver(b, &bm, &bn, &bp);
    if (am != bm) return am - bm;
    if (an != bn) return an - bn;
    return ap - bp;
}

// -- Hex helpers -----------------------------------------------------------

static bool hex_eq_lower(const char *a_lower, const uint8_t *bytes, size_t n)
{
    static const char H[] = "0123456789abcdef";
    char buf[129];
    if (n * 2 + 1 > sizeof(buf)) return false;
    for (size_t i = 0; i < n; i++) {
        buf[2 * i]     = H[(bytes[i] >> 4) & 0xF];
        buf[2 * i + 1] = H[ bytes[i]       & 0xF];
    }
    buf[n * 2] = '\0';
    return strcasecmp(a_lower, buf) == 0;
}

// -- Check task ------------------------------------------------------------

typedef struct {
    char url[192];
} check_arg_t;

static void check_task(void *arg)
{
    check_arg_t *ca = (check_arg_t *)arg;
    SK_LOG_I("ota", "check.start", "url=%s", ca->url);
    set_state(SK_OTA_CHECKING, "fetching manifest");

    esp_http_client_config_t cfg = {
        .url               = ca->url,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = OTA_HTTP_BUF_SIZE,
        .buffer_size_tx    = OTA_HTTP_BUF_SIZE,
        // We drive the request with open()/read() (streaming) so we can cap
        // the manifest size; that path does NOT auto-follow 3xx, so we chase
        // the Location header manually below.
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        SK_LOG_E("ota", "check.fail", "reason=http_init");
        set_state(SK_OTA_ERROR, "http init"); free(ca); vTaskDelete(NULL); return;
    }

    // GitHub serves `releases/.../download/manifest.json` as a 302 to a
    // cross-host CDN (release-assets.githubusercontent.com), often two hops
    // (latest → tag → asset). Follow up to 5 redirects; without this the
    // streaming fetch sees the 302 and aborts with "manifest http 302",
    // so the version comparison below never runs.
    int status = 0;
    bool got_response = false;
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) {
            SK_LOG_E("ota", "check.fail", "reason=http_open");
            set_state(SK_OTA_ERROR, "http open");
            esp_http_client_cleanup(c); free(ca); vTaskDelete(NULL); return;
        }
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            // Point the client at the Location header and reconnect.
            if (esp_http_client_set_redirection(c) != ESP_OK) break;
            esp_http_client_close(c);
            continue;
        }
        got_response = true;
        break;
    }
    if (!got_response || status != 200) {
        char m[64]; snprintf(m, sizeof(m), "manifest http %d", status);
        SK_LOG_E("ota", "check.fail", "reason=http_%d", status);
        set_state(SK_OTA_ERROR, m);
        esp_http_client_cleanup(c); free(ca); vTaskDelete(NULL); return;
    }

    char *buf = malloc(MANIFEST_BUF_SIZE);
    if (!buf) {
        SK_LOG_E("ota", "check.fail", "reason=alloc");
        set_state(SK_OTA_ERROR, "alloc"); esp_http_client_cleanup(c); free(ca);
        vTaskDelete(NULL); return;
    }
    int n = esp_http_client_read(c, buf, MANIFEST_BUF_SIZE - 1);
    esp_http_client_cleanup(c);
    free(ca);
    if (n <= 0) {
        SK_LOG_E("ota", "check.fail", "reason=empty_manifest");
        set_state(SK_OTA_ERROR, "empty manifest");
        free(buf); vTaskDelete(NULL); return;
    }
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        SK_LOG_E("ota", "check.fail", "reason=manifest_parse");
        set_state(SK_OTA_ERROR, "manifest parse");
        vTaskDelete(NULL); return;
    }

    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *u   = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *sha = cJSON_GetObjectItemCaseSensitive(root, "sha256");

    if (!cJSON_IsString(ver) || !cJSON_IsString(u)) {
        cJSON_Delete(root);
        SK_LOG_E("ota", "check.fail", "reason=manifest_missing_fields");
        set_state(SK_OTA_ERROR, "manifest missing version/url");
        vTaskDelete(NULL); return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strncpy(s_status.remote_version, ver->valuestring, sizeof(s_status.remote_version) - 1);
    s_status.remote_version[sizeof(s_status.remote_version) - 1] = '\0';
    strncpy(s_status.remote_url, u->valuestring, sizeof(s_status.remote_url) - 1);
    s_status.remote_url[sizeof(s_status.remote_url) - 1] = '\0';

    if (cJSON_IsString(sha) && strlen(sha->valuestring) == 64) {
        strncpy(s_status.remote_sha256, sha->valuestring, 64);
        s_status.remote_sha256[64] = '\0';
        s_status.has_sha256 = true;
    } else {
        s_status.remote_sha256[0] = '\0';
        s_status.has_sha256 = false;
    }
    xSemaphoreGive(s_mtx);
    cJSON_Delete(root);

    int cmp = semver_cmp(s_status.remote_version, s_status.current_version);
    if (cmp > 0) {
        SK_LOG_I("ota", "check.available", "current=%s available=%s",
                 s_status.current_version, s_status.remote_version);
        set_state(SK_OTA_UPDATE_AVAILABLE, "update available");
    } else {
        SK_LOG_I("ota", "check.up-to-date", "current=%s", s_status.current_version);
        set_state(SK_OTA_NO_UPDATE, "up to date");
    }
    vTaskDelete(NULL);
}

esp_err_t sk_ota_check(const char *custom_manifest_url)
{
    if (sk_ota_is_busy()) return ESP_ERR_INVALID_STATE;
    const char *url = custom_manifest_url ? custom_manifest_url : s_cfg_manifest_url;
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    check_arg_t *ca = malloc(sizeof(*ca));
    if (!ca) return ESP_ERR_NO_MEM;
    strncpy(ca->url, url, sizeof(ca->url) - 1);
    ca->url[sizeof(ca->url) - 1] = '\0';

    BaseType_t ok = xTaskCreate(check_task, "sk_ota_check", 6144, ca, 4, NULL);
    if (ok != pdPASS) { free(ca); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

// -- Download task ---------------------------------------------------------
//
// Uses step-by-step esp_https_ota so we can hash-verify the partition before
// activating it. Flow:
//   begin → perform-loop → (sha256 verify if has_sha256) → finish OR abort.

typedef struct {
    char url[192];
    bool verify_sha256;
    char expected_sha[65];
} download_arg_t;

static bool verify_partition_sha256(esp_https_ota_handle_t h, const char *expected_hex)
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p) return false;

    int total = esp_https_ota_get_image_size(h);
    if (total <= 0) return false;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        mbedtls_sha256_free(&sha); return false;
    }

    uint8_t *buf = malloc(HASH_READ_CHUNK);
    if (!buf) { mbedtls_sha256_free(&sha); return false; }

    int offset = 0;
    while (offset < total) {
        int to_read = (total - offset > HASH_READ_CHUNK) ? HASH_READ_CHUNK : (total - offset);
        if (esp_partition_read(p, offset, buf, to_read) != ESP_OK) {
            free(buf); mbedtls_sha256_free(&sha); return false;
        }
        mbedtls_sha256_update(&sha, buf, to_read);
        offset += to_read;
    }
    free(buf);

    uint8_t computed[32];
    mbedtls_sha256_finish(&sha, computed);
    mbedtls_sha256_free(&sha);
    return hex_eq_lower(expected_hex, computed, 32);
}

static void download_task(void *arg)
{
    download_arg_t *da = (download_arg_t *)arg;
    set_progress(0);
    set_state(SK_OTA_DOWNLOADING, "downloading");

    esp_http_client_config_t http_cfg = {
        .url               = da->url,
        .timeout_ms        = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = OTA_HTTP_BUF_SIZE,
        .buffer_size_tx    = OTA_HTTP_BUF_SIZE,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK || !h) {
        SK_LOG_E("ota", "install.fail.download", "reason=ota_begin err=%s",
                 esp_err_to_name(err));
        set_state(SK_OTA_ERROR, "ota begin");
        free(da); vTaskDelete(NULL); return;
    }

    int total = esp_https_ota_get_image_size(h);
    SK_LOG_I("ota", "install.start", "version=%s size=%d",
             s_status.remote_version, total);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (total > 0) {
            int read = esp_https_ota_get_image_len_read(h);
            int pct = (int)((int64_t)read * 100 / total);
            if (pct != s_status.progress_pct) set_progress(pct);
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        int bytes = esp_https_ota_get_image_len_read(h);
        SK_LOG_E("ota", "install.fail.download", "reason=%s bytes=%d",
                 esp_err_to_name(err), bytes);
        esp_https_ota_abort(h);
        set_state(SK_OTA_ERROR, esp_err_to_name(err));
        free(da); vTaskDelete(NULL); return;
    }

    if (da->verify_sha256) {
        set_state(SK_OTA_DOWNLOADING, "verifying sha256");
        if (!verify_partition_sha256(h, da->expected_sha)) {
            SK_LOG_E("ota", "install.fail.verify", "reason=sha256_mismatch");
            esp_https_ota_abort(h);
            set_state(SK_OTA_ERROR, "sha256 mismatch");
            free(da); vTaskDelete(NULL); return;
        }
    }

    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        SK_LOG_E("ota", "install.fail.verify", "reason=%s", esp_err_to_name(err));
        set_state(SK_OTA_ERROR, esp_err_to_name(err));
        free(da); vTaskDelete(NULL); return;
    }

    set_progress(100);
    SK_LOG_I("ota", "install.success", "version=%s reboot=imminent",
             s_status.remote_version);
    set_state(SK_OTA_DONE, "rebooting");
    sk_event_bus_publish("device.shutdown.imminent",
                         "{\"reason\":\"ota\",\"delay_ms\":1500}");
    free(da);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    vTaskDelete(NULL);
}

esp_err_t sk_ota_start(const char *custom_url)
{
    if (sk_ota_is_busy()) return ESP_ERR_INVALID_STATE;

    download_arg_t *da = malloc(sizeof(*da));
    if (!da) return ESP_ERR_NO_MEM;
    memset(da, 0, sizeof(*da));

    if (custom_url) {
        // APP-driven path — explicit URL override; sha256 unknown, skip verify.
        if (!custom_url[0]) { free(da); return ESP_ERR_INVALID_ARG; }
        strncpy(da->url, custom_url, sizeof(da->url) - 1);
        da->verify_sha256 = false;
    } else {
        // Cached-URL path — requires successful prior check.
        if (s_status.state != SK_OTA_UPDATE_AVAILABLE) { free(da); return ESP_ERR_INVALID_STATE; }
        if (!s_status.remote_url[0])                   { free(da); return ESP_ERR_INVALID_STATE; }
        strncpy(da->url, s_status.remote_url, sizeof(da->url) - 1);
        if (s_status.has_sha256) {
            da->verify_sha256 = true;
            strncpy(da->expected_sha, s_status.remote_sha256, sizeof(da->expected_sha) - 1);
        }
    }

    BaseType_t ok = xTaskCreate(download_task, "sk_ota_dl", DOWNLOAD_STACK,
                                da, 4, NULL);
    if (ok != pdPASS) { free(da); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t sk_ota_rollback(void)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (!other) return ESP_ERR_NOT_FOUND;
    esp_app_desc_t other_desc = {0};
    const char *to_ver = "?";
    if (esp_ota_get_partition_description(other, &other_desc) == ESP_OK) {
        to_ver = other_desc.version;
    }
    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err == ESP_OK) {
        SK_LOG_I("ota", "rollback", "from=%s to=%s",
                 s_status.current_version, to_ver);
        sk_event_bus_publish("device.shutdown.imminent",
                             "{\"reason\":\"ota_rollback\",\"delay_ms\":500}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return err;
}

// -- CLI handlers ----------------------------------------------------------

static sk_err_t cmd_ota_status(sk_cli_ctx_t *ctx)
{
    sk_ota_status_t st;
    sk_ota_get_status(&st);
    char buf[416];
    char masked_sha[16] = "";
    if (st.has_sha256) {
        strncpy(masked_sha, st.remote_sha256, 8);
        masked_sha[8] = '\0';
    }
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"progress\":%d,\"current\":\"%s\",\"remote\":\"%s\","
             "\"running_partition\":\"%s\",\"can_rollback\":%s,"
             "\"has_remote_url\":%s,\"has_sha256\":%s,\"sha256_prefix\":\"%s\","
             "\"message\":\"%s\"}",
             sk_ota_state_str(st.state), st.progress_pct,
             st.current_version, st.remote_version,
             sk_ota_running_label(),
             sk_ota_can_rollback() ? "true" : "false",
             st.remote_url[0] ? "true" : "false",
             st.has_sha256    ? "true" : "false",
             masked_sha,
             st.message);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static sk_err_t cmd_ota_check(sk_cli_ctx_t *ctx)
{
    const char *url = sk_cli_arg_named(ctx, "url");   // optional override
    esp_err_t err = sk_ota_check(url);
    if (err == ESP_ERR_INVALID_STATE) { sk_cli_err(ctx, SK_ERR_OTA_IN_PROGRESS, NULL); return SK_OK; }
    if (err == ESP_ERR_INVALID_ARG)   { sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"reason\":\"no_manifest_url\"}"); return SK_OK; }
    if (err != ESP_OK)                { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK; }
    sk_cli_ok(ctx, "{\"checking\":true}");
    return SK_OK;
}

static sk_err_t cmd_ota_start(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    const char *url = sk_cli_arg_named(ctx, "url");   // optional override
    esp_err_t err = sk_ota_start(url);
    if (err == ESP_ERR_INVALID_STATE) {
        sk_cli_err(ctx, SK_ERR_NO_UPDATE,
                   "{\"reason\":\"no_cached_update_run_check_first\"}");
        return SK_OK;
    }
    if (err != ESP_OK) { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK; }
    sk_cli_ok(ctx, "{\"started\":true}");
    return SK_OK;
}

static sk_err_t cmd_ota_rollback(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    if (!sk_ota_can_rollback()) { sk_cli_err(ctx, SK_ERR_NO_ROLLBACK, NULL); return SK_OK; }
    esp_err_t err = sk_ota_rollback();
    if (err != ESP_OK) { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK; }
    sk_cli_ok(ctx, "{\"rolling_back\":true}");
    return SK_OK;
}

// cmd_ota_partition REMOVED — partition info is already part of ota.status.
// `sk_ota_running_label()` is still exported for any non-CLI consumer that
// wants to query the active partition programmatically.

static const sk_cli_command_t s_cmds[] = {
    // Flat ota.* namespace (was ota.fw.*). Update server URL is preconfigured;
    // user never types it. Order = user journey: status → check → update,
    // plus rollback safety net. ota.fw.partition removed — partition info is
    // already in ota.status.
    { .name = "ota.status",
      .summary = "Show update state, current/available versions, active partition",
      .usage   = "ota status",
      .help_block =
          "Returns a snapshot of the firmware-update subsystem:\n"
          "  state              idle | checking | update_available | no_update |\n"
          "                     downloading | done | error\n"
          "  current            firmware version currently running\n"
          "  remote             version found by the last `ota check` (if any)\n"
          "  running_partition  active slot — factory | ota_0 | ota_1\n"
          "  can_rollback       whether a previous image exists to roll back to\n"
          "  has_sha256         true if the staged update carries a hash to verify\n"
          "  sha256_prefix      first 8 hex chars of that hash (when present)\n"
          "  message            human message from the previous attempt\n"
          "\n"
          "Read-only — never triggers a download or a reboot.\n"
          "\n"
          "Example:\n"
          "  ota status",
      .handler = cmd_ota_status },

    { .name = "ota.check",
      .summary = "Ask the update server if a newer firmware exists (no install)",
      .usage   = "ota check",
      .help_block =
          "Step 1 of the OTA flow. Contacts the preconfigured update server,\n"
          "compares the latest published version with what you're running,\n"
          "and remembers the result for the next `ota update` call.\n"
          "\n"
          "Safe: only reads. Nothing is flashed, the device does NOT reboot.\n"
          "Run this any time to find out whether an update is waiting.\n"
          "\n"
          "Example:\n"
          "  ota check",
      .handler = cmd_ota_check },

    { .name = "ota.update",
      .summary = "Install the new firmware found by `ota check` (reboots automatically)",
      .usage   = "ota update",
      .critical = true,
      .help_block =
          "Step 2 of the OTA flow. Downloads the firmware identified by the\n"
          "previous `ota check`, verifies its sha256, writes it to the\n"
          "inactive OTA partition, then reboots into the new image.\n"
          "\n"
          "If the freshly booted firmware doesn't mark itself valid within a\n"
          "few seconds, the bootloader automatically rolls back to the\n"
          "previous partition — bricking the device is very hard.\n"
          "\n"
          "Run `ota check` first; this command refuses if no update is staged.\n"
          "\n"
          "WARNING: device reboots when install completes. Critical — see\n"
          "`help device.confirm-token` for the confirm flow.",
      .handler = cmd_ota_start },

    { .name = "ota.rollback",
      .summary = "Boot back into the previous firmware (use if a new build is broken)",
      .usage   = "ota rollback",
      .critical = true,
      .help_block =
          "Switches the boot partition to the previously running firmware\n"
          "and reboots. Use this when an OTA update completed but the new\n"
          "build misbehaves and you can still reach the CLI.\n"
          "\n"
          "Has no effect on factory images (nothing to roll back to).\n"
          "Critical — see `help device.confirm-token` for the confirm flow.",
      .handler = cmd_ota_rollback },

    // ota.fw.partition REMOVED — partition info is already part of ota.status.
    // Keeping the handler around is harmless (dead code) but the registration
    // is gone so it won't appear in help/device.commands.
};

// -- Init ------------------------------------------------------------------

esp_err_t sk_ota_init(const sk_ota_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;

    if (cfg->fw_version) {
        strncpy(s_cfg_fw_version, cfg->fw_version, sizeof(s_cfg_fw_version) - 1);
        strncpy(s_status.current_version, cfg->fw_version, sizeof(s_status.current_version) - 1);
    }
    if (cfg->manifest_url) {
        strncpy(s_cfg_manifest_url, cfg->manifest_url, sizeof(s_cfg_manifest_url) - 1);
    }
    s_cfg = *cfg;

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        sk_cli_register(&s_cmds[i]);
    }
    sk_capabilities_register_book("sk_ota", "0.2.0");

    // Mark current image valid so the bootloader doesn't roll back on the
    // next reboot. Idempotent — safe on every boot.
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "sk_ota ready (current=%s, manifest=%s)",
             s_status.current_version,
             s_cfg_manifest_url[0] ? s_cfg_manifest_url : "<unset>");
    return ESP_OK;
}
