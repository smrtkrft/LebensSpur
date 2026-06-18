// sk_baseline.c — APP-facing baseline commands per shared/cli_contract.md §3.
//
// Six commands registered here:
//   device.info       — model/serial/protocol_version/...
//   device.commands   — JSON array of every registered command name
//   device.status     — uptime + wifi + ble + time (battery added later by hook)
//   device.manifest   — runtime UI manifest (commands + summaries + group hint)
//   logs.get          — ring buffer dump (stub: returns []; ring buffer TBD)
//   time.set          — APP pushes UNIX time when device has no NTP
//
// Implementation note: we hand-roll JSON via snprintf instead of cJSON to
// keep this file small and avoid heap fragmentation on long-running BLE
// links. Buffers are sized for typical responses; truncation is detected
// and surfaced as ERR_INTERNAL rather than silently corrupting JSON.

#include "sk_baseline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sk_cli.h"
#include "sk_identity.h"
#include "sk_errors.h"
#include "sk_wifi.h"
#include "sk_auth.h"
#include "sk_passphrase.h"
#include "sk_event_bus.h"
#include "sk_log.h"

static const char *TAG = "sk_baseline";

// === Storage ================================================================

static const char *s_fw_version = "";
static const char *s_build_info = NULL;
static int64_t     s_boot_us    = 0;

// user_configured: persistent boolean. Set the first time the device
// successfully reaches an IP (taken as proof that the user finished the
// initial setup wizard); cleared on factory reset. SKAPP reads this from
// device.info to decide whether to show the WiFi setup wizard after BLE
// pairing — if true, the device is already configured and SKAPP can
// route straight to the device home screen (re-pair / orphan-bond recovery
// flow).
#define DEVST_NVS_NS    "sk_devst"
#define DEVST_NVS_KEY   "user_cfg"
static bool s_user_configured = false;

static void user_configured_load(void)
{
    nvs_handle_t h;
    if (nvs_open(DEVST_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        s_user_configured = false;
        return;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, DEVST_NVS_KEY, &v) == ESP_OK) {
        s_user_configured = (v != 0);
    }
    nvs_close(h);
}

static void user_configured_persist(bool v)
{
    nvs_handle_t h;
    if (nvs_open(DEVST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "user_configured persist: nvs_open failed");
        return;
    }
    nvs_set_u8(h, DEVST_NVS_KEY, v ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void on_wifi_ip_acquired(const sk_event_t *evt, void *ctx)
{
    (void)evt; (void)ctx;
    if (s_user_configured) return;   // idempotent: only persist the transition
    s_user_configured = true;
    user_configured_persist(true);
    ESP_LOGI(TAG, "user_configured -> true (first IP)");
}

static void on_factory_reset(const sk_event_t *evt, void *ctx)
{
    (void)evt; (void)ctx;
    // Wipe entire namespace; cheaper than a single-key erase and aligns
    // with how other sk_* components treat the factory-reset event.
    nvs_handle_t h;
    if (nvs_open(DEVST_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_user_configured = false;
    ESP_LOGI(TAG, "user_configured -> false (factory reset)");
}

// === device.info ============================================================
//
// Merged form — used to be split as `device.info` (static identity) plus
// `device.status` (runtime). Two near-identical commands confused users;
// SKAPP also wanted "tell me everything" anyway. Single command now.

static int64_t uptime_sec(void);   // forward — defined alongside removed cmd

static sk_err_t cmd_device_info(sk_cli_ctx_t *ctx)
{
    sk_wifi_status_t w;
    sk_wifi_status(&w);

    // Lazy upgrade migration: a firmware upgraded from a build that
    // didn't track user_configured will have the flag false even though
    // WiFi credentials are saved (i.e. the user definitely went through
    // setup). The first device.info call after upgrade backfills it from
    // the WiFi credential presence, so SKAPP doesn't push an upgraded
    // user back through the wizard.
    if (!s_user_configured && (sk_wifi_has_primary() || sk_wifi_has_backup())) {
        s_user_configured = true;
        user_configured_persist(true);
    }

    time_t now    = time(NULL);
    bool   tvalid = now > 1700000000;   // any post-2023-11-15 epoch counts as set

    sk_pass_mode_t pmode = sk_passphrase_get_mode();
    bool pset = sk_passphrase_is_set();

    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        // identity (static)
        "{\"model\":\"%s\","
        "\"prefix\":\"%s\","
        "\"serial\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"hw_version\":\"%c\","
        "\"protocol_version\":\"%s\","
        "\"build_info\":%s%s%s,"
        // runtime snapshot (formerly device.status)
        "\"uptime_sec\":%lld,"
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\"},"
        "\"ble\":{\"advertising\":false,\"paired_clients\":%u},"
        "\"battery\":{\"present\":false},"
        "\"last_error\":null,"
        "\"passphrase\":{\"set\":%s,\"mode\":{\"pairing\":%s,\"always\":%s},\"attempts_left\":%u},"
        "\"bonds\":{\"count\":%u,\"capacity\":%u},"
        "\"user_configured\":%s,"
        "\"time\":{\"valid\":%s,\"unix\":%lld,\"source\":\"%s\"}}",
        sk_identity_get_prefix(),
        sk_identity_get_prefix(),
        sk_identity_get(),
        s_fw_version,
        sk_identity_get_hw_rev(),
        SK_PROTOCOL_VERSION,
        s_build_info ? "\"" : "null",
        s_build_info ? s_build_info : "",
        s_build_info ? "\"" : "",
        (long long)uptime_sec(),
        w.connected ? "true" : "false",
        w.ssid, w.rssi, w.ip,
        (unsigned)sk_auth_bond_count(),
        pset ? "true" : "false",
        pmode.pairing_required ? "true" : "false",
        pmode.always_required  ? "true" : "false",
        (unsigned)sk_passphrase_attempts_left(),
        (unsigned)sk_auth_bond_count(),
        (unsigned)SK_AUTH_BOND_SLOT_COUNT,
        s_user_configured ? "true" : "false",
        tvalid ? "true" : "false",
        (long long)(tvalid ? now : 0),
        tvalid ? "set" : "none");
    if (n < 0 || n >= (int)sizeof(buf)) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"info_truncated\"}");
        return SK_OK;
    }
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmd_device_info = {
    .name       = "device.info",
    .summary    = "Identity + runtime snapshot (merged info+status)",
    .usage      = "device info",
    .help_block =
        "Returns a single JSON snapshot covering both static identity and\n"
        "live runtime state. Fields:\n"
        "  identity        prefix + serial (e.g. BF-A1B2C3)\n"
        "  fw_version      firmware semver\n"
        "  protocol        SK protocol version\n"
        "  build_info      commit hash + build date\n"
        "  uptime_sec      seconds since boot\n"
        "  wifi            { connected, ssid, ip, rssi }\n"
        "  ble             { advertising, paired_clients }\n"
        "  battery         { present, ... }\n"
        "  user_configured true once the initial setup wizard ran\n"
        "  time            { valid, unix, source }",
    .handler    = cmd_device_info,
};

// === device.commands ========================================================

typedef struct {
    char  *out;
    size_t cap;
    size_t off;
    bool   first;
    bool   overflow;
} cmd_walk_state_t;

static void cmd_walk_collect(const sk_cli_command_t *cmd, void *user)
{
    cmd_walk_state_t *st = (cmd_walk_state_t *)user;
    if (st->overflow) return;
    int n = snprintf(st->out + st->off, st->cap - st->off,
                     "%s\"%s\"", st->first ? "" : ",", cmd->name);
    if (n < 0 || (size_t)n >= st->cap - st->off) {
        st->overflow = true;
        return;
    }
    st->off  += (size_t)n;
    st->first = false;
}

static sk_err_t cmd_device_commands(sk_cli_ctx_t *ctx)
{
    enum { BUF = 2048 };
    char *buf = malloc(BUF);
    if (!buf) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"oom\"}");
        return SK_OK;
    }
    cmd_walk_state_t st = { .out = buf, .cap = BUF, .off = 0, .first = true, .overflow = false };
    int n = snprintf(buf + st.off, BUF - st.off, "[");
    st.off += (size_t)n;
    sk_cli_walk(cmd_walk_collect, &st);
    if (st.overflow) {
        free(buf);
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"commands_truncated\"}");
        return SK_OK;
    }
    n = snprintf(buf + st.off, BUF - st.off, "]");
    if (n < 0 || (size_t)n >= BUF - st.off) {
        free(buf);
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"commands_truncated\"}");
        return SK_OK;
    }
    sk_cli_ok(ctx, buf);
    free(buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmd_device_commands = {
    .name       = "device.commands",
    .summary    = "List every registered command (JSON array)",
    .usage      = "device commands",
    .hidden     = true,   // SKAPP-only: humans use `help` / `help all`
    .help_block = "Machine-readable list. Hidden commands are included.\n"
                  "APP gates UI by what appears here, not by semver alone.",
    .handler    = cmd_device_commands,
};

// === uptime helper (formerly part of device.status, now used by device.info) ==

static int64_t uptime_sec(void)
{
    int64_t now = esp_timer_get_time();
    return (now - s_boot_us) / 1000000LL;
}

#if 0   // device.status REMOVED — merged into device.info
static sk_err_t cmd_device_status(sk_cli_ctx_t *ctx)
{
    sk_wifi_status_t w;
    sk_wifi_status(&w);

    time_t now    = time(NULL);
    bool   tvalid = now > 1700000000;  // ~2023-11-15: any later epoch counts as set

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"uptime_sec\":%lld,"
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\"},"
        "\"ble\":{\"advertising\":false,\"paired_clients\":0},"
        "\"battery\":{\"present\":false},"
        "\"last_error\":null,"
        "\"time\":{\"valid\":%s,\"unix\":%lld,\"source\":\"%s\"}}",
        (long long)uptime_sec(),
        w.connected ? "true" : "false",
        w.ssid,
        w.rssi,
        w.ip,
        tvalid ? "true" : "false",
        (long long)(tvalid ? now : 0),
        tvalid ? "set" : "none");
    if (n < 0 || n >= (int)sizeof(buf)) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"status_truncated\"}");
        return SK_OK;
    }
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmd_device_status = {
    .name       = "device.status",
    .summary    = "Runtime state (uptime, wifi, ble, time)",
    .usage      = "device status",
    .help_block = "Snapshot of runtime telemetry. Battery/extra fields are\n"
                  "TODO — added when bf_battery exposes a status hook.",
    .handler    = cmd_device_status,
};
#endif  // device.status REMOVED

// === device.manifest ========================================================

typedef struct {
    char  *out;
    size_t cap;
    size_t off;
    bool   first;
    bool   overflow;
} manifest_state_t;

static void manifest_walk_cb(const sk_cli_command_t *cmd, void *user)
{
    manifest_state_t *st = (manifest_state_t *)user;
    if (st->overflow) return;
    if (cmd->hidden) return;  // omit debug-only commands from the manifest

    // Group hint: take the first dot segment of the command name. APP can
    // bucket related commands together without us inventing a richer schema.
    const char *dot   = strchr(cmd->name, '.');
    int         glen  = dot ? (int)(dot - cmd->name) : (int)strlen(cmd->name);

    int n = snprintf(st->out + st->off, st->cap - st->off,
        "%s{\"name\":\"%s\",\"group\":\"%.*s\",\"summary\":\"%s\"}",
        st->first ? "" : ",",
        cmd->name,
        glen, cmd->name,
        cmd->summary ? cmd->summary : "");
    if (n < 0 || (size_t)n >= st->cap - st->off) {
        st->overflow = true;
        return;
    }
    st->off  += (size_t)n;
    st->first = false;
}

static sk_err_t cmd_device_manifest(sk_cli_ctx_t *ctx)
{
    enum { BUF = 4096 };
    char *buf = malloc(BUF);
    if (!buf) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"oom\"}");
        return SK_OK;
    }
    size_t off = 0;
    int n = snprintf(buf + off, BUF - off,
        "{\"device\":{\"prefix\":\"%s\",\"serial\":\"%s\","
        "\"protocol_version\":\"%s\",\"fw_version\":\"%s\"},"
        "\"commands\":[",
        sk_identity_get_prefix(), sk_identity_get(),
        SK_PROTOCOL_VERSION, s_fw_version);
    if (n < 0 || (size_t)n >= BUF - off) {
        free(buf);
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"manifest_header\"}");
        return SK_OK;
    }
    off += (size_t)n;

    manifest_state_t st = { .out = buf, .cap = BUF, .off = off, .first = true, .overflow = false };
    sk_cli_walk(manifest_walk_cb, &st);
    if (st.overflow) {
        free(buf);
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"manifest_truncated\"}");
        return SK_OK;
    }

    // Events array is empty for now — sk_event_bus does not expose a public
    // catalog yet. APP subscribes by wildcard and learns events as they fire.
    n = snprintf(buf + st.off, BUF - st.off, "],\"events\":[]}");
    if (n < 0 || (size_t)n >= BUF - st.off) {
        free(buf);
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"manifest_footer\"}");
        return SK_OK;
    }
    sk_cli_ok(ctx, buf);
    free(buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmd_device_manifest = {
    .name       = "device.manifest",
    .summary    = "Runtime UI manifest for SKAPP",
    .usage      = "device manifest",
    .hidden     = true,   // SKAPP-only: humans don't read manifests
    .help_block = "Returns identity, the visible command list with group hints,\n"
                  "and (eventually) the event catalog. APP renders screens from\n"
                  "this; no static manifest file is needed.",
    .handler    = cmd_device_manifest,
};

// === logs.get ===============================================================
//
// Reads the structured event ring buffer maintained by sk_log (see
// sk_log.c). Returns JSON: {"lines":[{ts,lvl,tag,event,msg}, ...]}.
// Supports --limit (default 50, max 200) and --level (debug|info|warn|error).

#define SK_LOG_RING_LINE_ESC  300  // 128 raw msg + worst-case JSON escape headroom

// JSON-escape into out (NUL-terminated). Truncates if cap is too small.
static int json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (!in || cap == 0) { if (out && cap) out[0] = '\0'; return 0; }
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= cap) break;
            out[o++] = '\\'; out[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 7 >= cap) break;
            o += snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

typedef struct {
    char  *buf;
    size_t cap;
    size_t off;
    bool   first;
    bool   overflow;
} logs_walk_ctx_t;

static bool logs_walk_emit(const sk_log_entry_view_t *e, void *user) {
    logs_walk_ctx_t *st = (logs_walk_ctx_t *)user;
    if (st->overflow) return false;

    char msg_esc[SK_LOG_RING_LINE_ESC];
    json_escape(e->msg ? e->msg : "", msg_esc, sizeof(msg_esc));

    int n = snprintf(st->buf + st->off, st->cap - st->off,
                     "%s{\"ts\":%lld,\"up_us\":%lld,\"lvl\":\"%s\",\"tag\":\"%s\",\"event\":\"%s\",\"msg\":\"%s\"}",
                     st->first ? "" : ",",
                     (long long)e->ts_unix,
                     (long long)e->ts_uptime_us,
                     sk_log_level_str(e->level),
                     e->tag ? e->tag : "?",
                     e->event ? e->event : "?",
                     msg_esc);
    if (n < 0 || (size_t)n >= st->cap - st->off) {
        st->overflow = true;
        return false;
    }
    st->off  += (size_t)n;
    st->first = false;
    return true;
}

static sk_err_t cmd_logs_get(sk_cli_ctx_t *ctx) {
    // Parse --limit and --level (both optional).
    long lim_l = 50;
    sk_cli_arg_long(ctx, "limit", &lim_l);
    if (lim_l < 1)   lim_l = 1;
    if (lim_l > 200) lim_l = 200;
    size_t limit = (size_t)lim_l;

    sk_log_level_t min_level = SK_LOG_INFO;
    const char *lvl_str = sk_cli_arg_named(ctx, "level");
    if (lvl_str && !sk_log_level_parse(lvl_str, &min_level)) {
        sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"level\"}");
        return SK_OK;
    }

    enum { BUF_CAP = 8192 };
    char *buf = malloc(BUF_CAP);
    if (!buf) { sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK; }

    logs_walk_ctx_t st = {
        .buf = buf, .cap = BUF_CAP, .off = 0, .first = true, .overflow = false
    };
    int n = snprintf(buf + st.off, BUF_CAP - st.off, "{\"lines\":[");
    if (n < 0 || (size_t)n >= BUF_CAP - st.off) {
        free(buf); sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK;
    }
    st.off += (size_t)n;

    sk_log_walk(min_level, limit, logs_walk_emit, &st);

    n = snprintf(buf + st.off, BUF_CAP - st.off, "]%s}",
                 st.overflow ? ",\"truncated\":true" : "");
    if (n < 0 || (size_t)n >= BUF_CAP - st.off) {
        free(buf); sk_cli_err(ctx, SK_ERR_INTERNAL, NULL); return SK_OK;
    }
    sk_cli_ok(ctx, buf);
    free(buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmd_logs_get = {
    .name       = "logs.get",
    .summary    = "Read recent log entries from the ring buffer",
    .usage      = "logs get [--limit N] [--level LVL]",
    .help_block =
        "Returns structured event entries from the on-device ring buffer.\n"
        "Both CLI and SKAPP use this for diagnostics: what happened on the\n"
        "device just before the issue.\n"
        "\n"
        "--limit  number of entries to return (default 50, max 200)\n"
        "--level  minimum severity: debug | info | warn | error (default info)\n"
        "\n"
        "Each entry: {ts (unix), up_us (uptime us), lvl, tag, event, msg}.\n"
        "ts=0 means the device clock was not set when the entry was written.\n"
        "\n"
        "Examples:\n"
        "  logs get\n"
        "  logs get --limit 100\n"
        "  logs get --level warn",
    .handler    = cmd_logs_get,
};

// === time.set ===============================================================

static sk_err_t cmd_time_set(sk_cli_ctx_t *ctx)
{
    // Accept both `time set 1742000000` (positional, what the .usage doc
    // says) and `time set --unix 1742000000` (named, machine mode + scripts).
    // Earlier the handler only read --unix, so a user following the usage
    // line got SK_ERR_MISSING_ARG.
    const char *unix_str = sk_cli_arg_named(ctx, "unix");
    if (!unix_str && !sk_cli_is_machine_mode(ctx)) {
        unix_str = sk_cli_arg(ctx, 0);
    }
    if (!unix_str) {
        sk_cli_err(ctx, SK_ERR_MISSING_ARG, "{\"field\":\"unix\"}");
        return SK_OK;
    }
    long long unix_t = strtoll(unix_str, NULL, 10);
    if (unix_t < 1700000000LL) {  // sanity: pre-2023-11-15 is almost certainly a typo
        sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"unix\",\"reason\":\"too_low\"}");
        return SK_OK;
    }

    struct timeval tv = { .tv_sec = (time_t)unix_t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"settimeofday_failed\"}");
        return SK_OK;
    }

    char ack[64];
    snprintf(ack, sizeof(ack), "{\"unix\":%lld}", unix_t);
    sk_cli_ok(ctx, ack);
    ESP_LOGI(TAG, "time.set unix=%lld", unix_t);
    return SK_OK;
}

static const sk_cli_command_t s_cmd_time_set = {
    .name       = "time.set",
    .summary    = "Push UNIX time to device (SKAPP-only, when no NTP)",
    .usage      = "time set <unix>",
    .hidden     = true,   // SKAPP-only: human users don't push UTC epochs
    .help_block = "Sets the system clock to the given UTC unix timestamp. SKAPP\n"
                  "calls this on every connect before WiFi/NTP is available, so\n"
                  "log timestamps and webhook payloads carry real time.",
    .handler    = cmd_time_set,
};

// === init ===================================================================

esp_err_t sk_baseline_init(const char *fw_version, const char *build_info)
{
    s_fw_version = fw_version ? fw_version : "";
    s_build_info = (build_info && build_info[0]) ? build_info : NULL;
    s_boot_us    = esp_timer_get_time();

    user_configured_load();
    sk_event_bus_subscribe("wifi.ip.acquired",
                           on_wifi_ip_acquired, NULL, NULL);
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, NULL);

    sk_cli_register(&s_cmd_device_info);
    sk_cli_register(&s_cmd_device_commands);
    // s_cmd_device_status REMOVED — fields merged into device.info above.
    sk_cli_register(&s_cmd_device_manifest);
    sk_cli_register(&s_cmd_logs_get);
    // Structured event log (NimBLE-safe async queue). Replaces the old
    // esp_log_set_vprintf hook, which blocked the NimBLE host task and
    // had to be disabled. See esp32/COMMON_LOG_SPEC.md.
    if (sk_log_init() != ESP_OK) {
        ESP_LOGW(TAG, "sk_log_init failed; logs.get will be empty");
    }
    sk_cli_register(&s_cmd_time_set);

    ESP_LOGI(TAG, "baseline cmds registered (protocol %s)", SK_PROTOCOL_VERSION);
    return ESP_OK;
}
