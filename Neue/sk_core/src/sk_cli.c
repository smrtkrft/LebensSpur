#include "sk_cli.h"
#include "sk_cli_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sk_core.h"   // sk_core_write_banner for help header

static const char *TAG = "sk_cli";

#define SK_CLI_MAX_COMMANDS   128
#define SK_CLI_MAX_TOPICS     32
#define SK_CLI_MAX_ARGV       16
#define SK_CLI_LINE_BUF       1024

static const sk_cli_command_t *s_commands[SK_CLI_MAX_COMMANDS];
static int                     s_command_count = 0;

typedef struct {
    const char *name;
    const char *summary;
    const char *category;       // NULL = uncategorized
} sk_cli_topic_entry_t;

static sk_cli_topic_entry_t    s_topics[SK_CLI_MAX_TOPICS];
static int                     s_topic_count = 0;

static SemaphoreHandle_t       s_mtx           = NULL;
static sk_cli_mode_t           s_mode          = SK_CLI_MODE_HUMAN;
static bool                    s_ready         = false;
static sk_cli_confirm_issuer_t s_confirm_issuer = NULL;

esp_err_t sk_cli_set_confirm_issuer(sk_cli_confirm_issuer_t issuer)
{
    s_confirm_issuer = issuer;
    return ESP_OK;
}

// Forward declarations
static sk_err_t builtin_help(sk_cli_ctx_t *ctx);
static sk_err_t builtin_json_on(sk_cli_ctx_t *ctx);
static sk_err_t builtin_json_off(sk_cli_ctx_t *ctx);

static const sk_cli_command_t s_builtins[] = {
    { .name = "help",
      .summary  = "List commands or show detail of one",
      .usage    = "help [<command>]",
      .help_block = "help\n  Show the namespace list.\nhelp <command>\n  Show usage, parameters and example for a specific command.",
      .handler  = builtin_help },
    // json.on/off are protocol switches the SKAPP / clients flip
    // automatically. Humans interactively never type them, so they're
    // hidden from the help overview but still callable by name.
    { .name = "json.on",
      .summary  = "Switch to NDJSON machine mode",
      .usage    = "json on",
      .hidden   = true,
      .handler  = builtin_json_on },
    { .name = "json.off",
      .summary  = "Switch to human mode",
      .usage    = "json off",
      .hidden   = true,
      .handler  = builtin_json_off },
};

esp_err_t sk_cli_init(void)
{
    if (s_ready) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;
    s_command_count = 0;
    s_ready = true;
    for (size_t i = 0; i < sizeof(s_builtins)/sizeof(s_builtins[0]); i++) {
        sk_cli_register(&s_builtins[i]);
    }
    return ESP_OK;
}

esp_err_t sk_cli_register(const sk_cli_command_t *cmd)
{
    if (!s_ready || !cmd || !cmd->name || !cmd->handler) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_command_count >= SK_CLI_MAX_COMMANDS) {
        xSemaphoreGive(s_mtx);
        ESP_LOGE(TAG, "command table full registering %s", cmd->name);
        return ESP_ERR_NO_MEM;
    }
    // Replace on duplicate name so devices can override library defaults.
    for (int i = 0; i < s_command_count; i++) {
        if (strcmp(s_commands[i]->name, cmd->name) == 0) {
            s_commands[i] = cmd;
            xSemaphoreGive(s_mtx);
            return ESP_OK;
        }
    }
    s_commands[s_command_count++] = cmd;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

void sk_cli_set_mode(sk_cli_mode_t mode) { s_mode = mode; }
sk_cli_mode_t sk_cli_get_mode(void)      { return s_mode; }

const sk_cli_command_t *sk_cli_lookup(const char *name)
{
    for (int i = 0; i < s_command_count; i++) {
        if (strcmp(s_commands[i]->name, name) == 0) return s_commands[i];
    }
    return NULL;
}

void sk_cli_walk(sk_cli_walk_cb_t cb, void *user)
{
    if (!cb) return;
    for (int i = 0; i < s_command_count; i++) {
        cb(s_commands[i], user);
    }
}

esp_err_t sk_cli_register_topic(const char *topic,
                                const char *summary,
                                const char *category)
{
    if (!s_ready || !topic || !topic[0]) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    // Replace if topic already registered.
    for (int i = 0; i < s_topic_count; i++) {
        if (strcmp(s_topics[i].name, topic) == 0) {
            s_topics[i].summary  = summary;
            s_topics[i].category = category;
            xSemaphoreGive(s_mtx);
            return ESP_OK;
        }
    }
    if (s_topic_count >= SK_CLI_MAX_TOPICS) {
        xSemaphoreGive(s_mtx);
        ESP_LOGE(TAG, "topic table full registering %s", topic);
        return ESP_ERR_NO_MEM;
    }
    s_topics[s_topic_count].name     = topic;
    s_topics[s_topic_count].summary  = summary;
    s_topics[s_topic_count].category = category;
    s_topic_count++;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

const char *sk_cli_topic_summary(const char *topic)
{
    if (!topic) return NULL;
    for (int i = 0; i < s_topic_count; i++) {
        if (strcmp(s_topics[i].name, topic) == 0) return s_topics[i].summary;
    }
    return NULL;
}

const char *sk_cli_topic_category(const char *topic)
{
    if (!topic) return NULL;
    for (int i = 0; i < s_topic_count; i++) {
        if (strcmp(s_topics[i].name, topic) == 0) return s_topics[i].category;
    }
    return NULL;
}

// -- Writer helpers ---------------------------------------------------------

void sk_cli_write(sk_cli_ctx_t *ctx, const char *chunk, size_t len)
{
    if (!ctx || !ctx->writer || !chunk) return;
    if (len == 0) len = strlen(chunk);
    ctx->writer(chunk, len, ctx->writer_user);
}

void sk_cli_writef(sk_cli_ctx_t *ctx, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    // vsnprintf returns the *desired* length, not the actual one. On
    // truncation buf still contains only sizeof(buf)-1 chars + null, so
    // we MUST clamp before passing to the writer — otherwise the writer
    // reads past the buffer into stack garbage. Caller-side problem
    // surfaced as random bytes mid-help_block when help text exceeded
    // 512 bytes.
    if ((size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
    sk_cli_write(ctx, buf, (size_t)n);
}

bool sk_cli_is_machine_mode(sk_cli_ctx_t *ctx) { return ctx && ctx->is_machine_mode; }
int  sk_cli_argc(sk_cli_ctx_t *ctx)            { return ctx ? ctx->human_argc : 0; }

const char *sk_cli_arg(sk_cli_ctx_t *ctx, int idx)
{
    if (!ctx || idx < 0 || idx >= ctx->human_argc) return NULL;
    return ctx->human_argv[idx];
}

const char *sk_cli_arg_named(sk_cli_ctx_t *ctx, const char *key)
{
    if (!ctx || !key) return NULL;
    if (ctx->is_machine_mode) {
        if (!ctx->machine_args) return NULL;
        cJSON *v = cJSON_GetObjectItemCaseSensitive(ctx->machine_args, key);
        if (!v) return NULL;
        if (cJSON_IsString(v)) return v->valuestring;
        return NULL;  // numeric values: handlers should read via args directly
    } else {
        // Look for --key value pairs in human argv.
        char flag[48];
        snprintf(flag, sizeof(flag), "--%s", key);
        for (int i = 0; i < ctx->human_argc - 1; i++) {
            if (strcmp(ctx->human_argv[i], flag) == 0) return ctx->human_argv[i + 1];
        }
        return NULL;
    }
}

bool sk_cli_arg_long(sk_cli_ctx_t *ctx, const char *key, long *out_value)
{
    if (!ctx || !key || !out_value) return false;

    if (ctx->is_machine_mode) {
        if (!ctx->machine_args) return false;
        cJSON *v = cJSON_GetObjectItemCaseSensitive(ctx->machine_args, key);
        if (!v) return false;
        // Native JSON numbers (SKAPP's typical shape: `{"offset":42}`) are
        // the primary case — read them straight off the parsed tree
        // without re-stringifying. Numeric strings (`{"offset":"42"}`)
        // and human mode --key 42 fall through to strtol.
        if (cJSON_IsNumber(v)) {
            *out_value = (long)v->valuedouble;
            return true;
        }
        if (cJSON_IsString(v) && v->valuestring) {
            char *end = NULL;
            long n = strtol(v->valuestring, &end, 10);
            if (end != v->valuestring) { *out_value = n; return true; }
        }
        return false;
    }

    // Human mode: --key <number>
    const char *s = sk_cli_arg_named(ctx, key);
    if (!s) return false;
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (end == s) return false;
    *out_value = n;
    return true;
}

const char *sk_cli_arg_after(sk_cli_ctx_t *ctx, const char *keyword)
{
    if (!ctx || !keyword || !keyword[0]) return NULL;

    // Machine mode: fall back to JSON key lookup so handlers can use one
    // helper across modes. Same semantics as sk_cli_arg_named for strings.
    if (ctx->is_machine_mode) {
        if (!ctx->machine_args) return NULL;
        cJSON *v = cJSON_GetObjectItemCaseSensitive(ctx->machine_args, keyword);
        if (cJSON_IsString(v)) return v->valuestring;
        return NULL;
    }

    // Human mode: scan argv pairwise. The keyword must NOT be the last
    // token (we need argv[i+1] to be the value).
    for (int i = 0; i < ctx->human_argc - 1; i++) {
        if (strcmp(ctx->human_argv[i], keyword) == 0) {
            return ctx->human_argv[i + 1];
        }
    }
    return NULL;
}

bool sk_cli_arg_after_long(sk_cli_ctx_t *ctx, const char *keyword, long *out)
{
    if (!ctx || !keyword || !out) return false;

    // Machine mode: accept native JSON numbers in addition to strings.
    if (ctx->is_machine_mode) {
        if (!ctx->machine_args) return false;
        cJSON *v = cJSON_GetObjectItemCaseSensitive(ctx->machine_args, keyword);
        if (!v) return false;
        if (cJSON_IsNumber(v)) { *out = (long)v->valuedouble; return true; }
        if (cJSON_IsString(v) && v->valuestring) {
            char *end = NULL;
            long n = strtol(v->valuestring, &end, 10);
            if (end != v->valuestring) { *out = n; return true; }
        }
        return false;
    }

    const char *s = sk_cli_arg_after(ctx, keyword);
    if (!s) return false;
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (end == s) return false;
    *out = n;
    return true;
}

const char *sk_cli_confirm_token(sk_cli_ctx_t *ctx) { return ctx ? ctx->confirm_token : NULL; }
bool        sk_cli_is_authenticated(sk_cli_ctx_t *ctx) { return ctx && ctx->authenticated; }

// -- Usage / structured output helpers --------------------------------------

// Escape a free-text string for embedding in a JSON string literal. Writes
// at most cap-1 bytes plus NUL terminator. Used only by sk_cli_usage's
// machine-mode branch; not a general-purpose escaper.
static size_t json_escape_into(char *out, size_t cap, const char *in)
{
    if (!out || cap == 0) return 0;
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        const char *esc = NULL;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default: break;
        }
        if (esc) {
            if (o + 2 >= cap) break;
            out[o++] = esc[0];
            out[o++] = esc[1];
        } else if (c < 0x20) {
            if (o + 6 >= cap) break;
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

void sk_cli_usage(sk_cli_ctx_t *ctx,
                  const char  *usage,
                  const char  *params_desc,
                  const char  *example)
{
    if (!ctx) return;

    if (ctx->is_machine_mode) {
        // Build {usage,params,example} JSON safely. Each field gets a
        // bounded scratch buffer; longer inputs are truncated rather
        // than dropped (still useful for the client).
        char u_buf[256] = {0};
        char p_buf[512] = {0};
        char e_buf[256] = {0};
        if (usage)       json_escape_into(u_buf, sizeof(u_buf), usage);
        if (params_desc) json_escape_into(p_buf, sizeof(p_buf), params_desc);
        if (example)     json_escape_into(e_buf, sizeof(e_buf), example);

        char params[1100];
        size_t off = 0;
        off += (size_t)snprintf(params + off, sizeof(params) - off, "{");
        bool first = true;
        if (usage) {
            off += (size_t)snprintf(params + off, sizeof(params) - off,
                                    "\"usage\":\"%s\"", u_buf);
            first = false;
        }
        if (params_desc) {
            off += (size_t)snprintf(params + off, sizeof(params) - off,
                                    "%s\"params\":\"%s\"", first ? "" : ",", p_buf);
            first = false;
        }
        if (example) {
            off += (size_t)snprintf(params + off, sizeof(params) - off,
                                    "%s\"example\":\"%s\"", first ? "" : ",", e_buf);
            first = false;
        }
        if (off + 1 < sizeof(params)) {
            params[off++] = '}';
            params[off]   = '\0';
        } else {
            params[sizeof(params) - 2] = '}';
            params[sizeof(params) - 1] = '\0';
        }
        sk_cli_err(ctx, SK_ERR_MISSING_ARG, params);
        return;
    }

    // Human mode: pretty multi-line hint. No leading "error:" — caller
    // decides whether to treat this as an error (return non-SK_OK) or
    // just informational output.
    if (usage && usage[0]) {
        sk_cli_write(ctx, "Usage: ", 0);
        sk_cli_write(ctx, usage, 0);
        sk_cli_write(ctx, "\n", 1);
    }
    if (params_desc && params_desc[0]) {
        // Indent each line by two spaces. params_desc may already contain
        // newlines for multi-parameter hints.
        const char *p = params_desc;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            sk_cli_write(ctx, "  ", 2);
            sk_cli_write(ctx, p, len);
            sk_cli_write(ctx, "\n", 1);
            if (!nl) break;
            p = nl + 1;
        }
    }
    if (example && example[0]) {
        // Each \n-separated example line becomes its own "Example: " row.
        const char *p = example;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            sk_cli_write(ctx, "Example: ", 0);
            sk_cli_write(ctx, p, len);
            sk_cli_write(ctx, "\n", 1);
            if (!nl) break;
            p = nl + 1;
        }
    }
}

void sk_cli_kv(sk_cli_ctx_t *ctx, const char *label, const char *value)
{
    if (!ctx || ctx->is_machine_mode) return;
    if (!label) label = "";
    if (!value) value = "";
    sk_cli_write(ctx, "  ", 2);
    sk_cli_write(ctx, label, 0);
    sk_cli_write(ctx, ": ", 2);
    sk_cli_write(ctx, value, 0);
    sk_cli_write(ctx, "\n", 1);
}

void sk_cli_kvf(sk_cli_ctx_t *ctx, const char *label, const char *fmt, ...)
{
    if (!ctx || ctx->is_machine_mode) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
    buf[n] = '\0';
    sk_cli_kv(ctx, label, buf);
}

size_t sk_cli_fmt_duration(char *out, size_t cap, uint32_t seconds)
{
    if (!out || cap == 0) return 0;
    if (seconds == 0) {
        int n = snprintf(out, cap, "0s");
        if (n < 0) { out[0] = '\0'; return 0; }
        return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
    }

    uint32_t days  = seconds / 86400U;
    uint32_t rem   = seconds % 86400U;
    uint32_t hours = rem / 3600U;
    rem            = rem % 3600U;
    uint32_t mins  = rem / 60U;
    uint32_t secs  = rem % 60U;

    size_t off = 0;
    out[0] = '\0';
    #define APPEND_PART(val, suffix)                                            \
        do {                                                                    \
            if ((val) > 0 && off < cap) {                                       \
                int _n = snprintf(out + off, cap - off,                         \
                                  "%s%u" suffix, (off ? " " : ""),              \
                                  (unsigned)(val));                             \
                if (_n < 0) break;                                              \
                if ((size_t)_n >= cap - off) { off = cap - 1; out[off] = '\0'; break; } \
                off += (size_t)_n;                                              \
            }                                                                   \
        } while (0)

    APPEND_PART(days,  "d");
    APPEND_PART(hours, "h");
    APPEND_PART(mins,  "m");
    APPEND_PART(secs,  "s");

    #undef APPEND_PART
    return off;
}

// -- Response envelopes -----------------------------------------------------

// Human-mode pretty-printer for the JSON-shaped data payload that handlers
// pass to sk_cli_ok. The JSON is the same string emitted in machine mode —
// we just re-render it onto multiple lines, drop the quotes around object
// keys and string values, and indent nested objects. Arrays stay inline
// because every status command in the tree currently uses arrays for short
// primitive lists (e.g. capability books, endpoint indices).
//
// Streaming, recursive descent. Inputs come from snprintf'd buffers in the
// handlers, never user-controlled text — so we trust that they're valid
// JSON and don't bother with full escape handling. \" and \\ inside string
// values are passed through as-is, which is acceptable because no current
// handler embeds quotes in user-visible strings.

static const char *pp_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static void pp_indent(sk_cli_ctx_t *ctx, int depth)
{
    for (int i = 0; i < depth; i++) sk_cli_write(ctx, "  ", 2);
}

// Write a JSON string token without its surrounding quotes. Returns a
// pointer past the closing quote.
static const char *pp_emit_string(sk_cli_ctx_t *ctx, const char *p)
{
    if (*p != '"') return p;
    p++;
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p += 2;
        else                    p++;
    }
    if (ctx) sk_cli_write(ctx, start, (size_t)(p - start));
    if (*p == '"') p++;
    return p;
}

// Numbers, true, false, null — emit verbatim until a structural delimiter.
static const char *pp_emit_primitive(sk_cli_ctx_t *ctx, const char *p)
{
    const char *start = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r')
        p++;
    if (ctx) sk_cli_write(ctx, start, (size_t)(p - start));
    return p;
}

static const char *pp_emit_value(sk_cli_ctx_t *ctx, const char *p, int depth);

// Emit each member of an object on its own line at `depth`. Caller has
// already produced the "  key: " prefix that introduces this object (or
// nothing, for the top-level object). `p` points to '{'.
static const char *pp_emit_object_members(sk_cli_ctx_t *ctx,
                                          const char *p, int depth)
{
    p++;                               // consume '{'
    p = pp_skip_ws(p);
    if (*p == '}') {
        if (ctx) sk_cli_write(ctx, "{}", 2);
        return p + 1;
    }
    while (*p && *p != '}') {
        p = pp_skip_ws(p);
        if (ctx) {
            sk_cli_write(ctx, "\n", 1);
            pp_indent(ctx, depth);
        }
        p = pp_emit_string(ctx, p);    // key
        p = pp_skip_ws(p);
        if (*p == ':') p++;
        p = pp_skip_ws(p);
        if (ctx) sk_cli_write(ctx, ": ", 2);
        // If the value is itself an object, recurse with deeper indent and
        // skip the inline ": " content; the recursive call emits each
        // member on its own line.
        if (*p == '{') {
            p = pp_emit_object_members(ctx, p, depth + 1);
        } else {
            p = pp_emit_value(ctx, p, depth);
        }
        p = pp_skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == '}') p++;
    return p;
}

static const char *pp_emit_array(sk_cli_ctx_t *ctx, const char *p, int depth)
{
    p++;                               // consume '['
    if (ctx) sk_cli_write(ctx, "[", 1);
    bool first = true;
    while (*p && *p != ']') {
        p = pp_skip_ws(p);
        if (*p == ']') break;
        if (!first && ctx) sk_cli_write(ctx, ", ", 2);
        first = false;
        p = pp_emit_value(ctx, p, depth);
        p = pp_skip_ws(p);
        if (*p == ',') p++;
    }
    if (*p == ']') p++;
    if (ctx) sk_cli_write(ctx, "]", 1);
    return p;
}

static const char *pp_emit_value(sk_cli_ctx_t *ctx, const char *p, int depth)
{
    p = pp_skip_ws(p);
    if (*p == '{') return pp_emit_object_members(ctx, p, depth + 1);
    if (*p == '[') return pp_emit_array(ctx, p, depth);
    if (*p == '"') return pp_emit_string(ctx, p);
    return pp_emit_primitive(ctx, p);
}

// Pretty-print one top-level JSON value onto the writer, followed by '\n'.
static void pp_render(sk_cli_ctx_t *ctx, const char *json)
{
    if (!json) return;
    const char *p = pp_skip_ws(json);
    if (*p == '{') {
        // Top-level object: members indented at depth 1, no enclosing braces.
        pp_emit_object_members(ctx, p, 1);
    } else {
        pp_emit_value(ctx, p, 0);
    }
    sk_cli_write(ctx, "\n", 1);
}

void sk_cli_ok(sk_cli_ctx_t *ctx, const char *data_json_or_null)
{
    if (!ctx) return;
    if (ctx->is_machine_mode) {
        // Split the envelope into pieces so an arbitrarily-large
        // `data_json_or_null` (e.g. 4 KB+ base64 from userdata.read) is not
        // squeezed through sk_cli_writef's 512-byte vsnprintf buffer.
        sk_cli_writef(ctx, "{\"id\":%d,\"ok\":true", ctx->machine_id);
        if (data_json_or_null) {
            sk_cli_write(ctx, ",\"data\":", 8);
            sk_cli_write(ctx, data_json_or_null, 0);
        }
        sk_cli_write(ctx, "}\n", 2);
    } else {
        sk_cli_write(ctx, "ok.", 3);
        if (data_json_or_null) {
            // pp_render emits its own leading newline (before the first key)
            // and a trailing '\n', so the result is a multi-line block.
            pp_render(ctx, data_json_or_null);
        } else {
            sk_cli_write(ctx, "\n", 1);
        }
    }
    ctx->wrote_envelope = true;
}

void sk_cli_err(sk_cli_ctx_t *ctx, sk_err_t err, const char *params_json_or_null)
{
    if (!ctx) return;
    const char *code = sk_err_code_string(err);
    const char *msg  = sk_err_message(err);
    if (ctx->is_machine_mode) {
        sk_cli_writef(ctx, "{\"id\":%d,\"ok\":false,\"err\":\"%s\"%s%s}\n",
                      ctx->machine_id, code,
                      params_json_or_null ? ",\"params\":" : "",
                      params_json_or_null ? params_json_or_null : "");
    } else {
        sk_cli_writef(ctx, "error: %s - %s\n", code, msg);
    }
    ctx->wrote_envelope = true;
}

// -- Tokenizer (human mode) -------------------------------------------------

static int tokenize(char *line, const char **out, int max_out)
{
    int n = 0;
    char *p = line;
    while (*p && n < max_out) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            out[n++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') { *p = '\0'; p++; }
        } else {
            out[n++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) { *p = '\0'; p++; }
        }
    }
    return n;
}

// =====================================================================
// Human-mode command resolution: longest-prefix match
// ---------------------------------------------------------------------
// Input is whitespace-separated argv. Commands are registered with
// dotted canonical names ("timer.set", "mail.group.recipient.add",
// "auth.passphrase.mode.set"). We must figure out how many leading argv
// elements form the command name and how many remain as arguments.
//
// Algorithm: try the longest prefix first, walking down to 1 token.
// For each `take` count from min(N, 6) down to 1, join argv[0..take-1]
// with '.' and look up the canonical name. The first hit wins.
// Longest-prefix-first guarantees that a fully-qualified subcommand
// (e.g. "mail group recipient add") matches the deepest registration
// rather than a shallower ancestor.
//
// Worked example: `mail group recipient add 0 alice@example.com`
//   tokens = ["mail","group","recipient","add","0","alice@example.com"]
//   take=6 → "mail.group.recipient.add.0.alice@example.com"  no match
//   take=5 → "mail.group.recipient.add.0"                    no match
//   take=4 → "mail.group.recipient.add"                      MATCH
//   consumed=4 → ctx->human_argv = ["0","alice@example.com"]
//
// Worked example: `timer set minute 2 alarm 1`
//   take=4 → "timer.set.minute.2"                            no match
//   take=3 → "timer.set.minute"                              no match
//   take=2 → "timer.set"                                     MATCH
//   consumed=2 → ctx->human_argv = ["minute","2","alarm","1"]
//   → sk_cli_arg(ctx,0)="minute", sk_cli_arg_after(ctx,"alarm")="1"
//
// The 6-segment cap (canonical buffer is 128 bytes) is enough for any
// realistic command name; the deepest existing commands are 4 segments.
// =====================================================================
static const sk_cli_command_t *resolve_human(const char **tokens, int token_count, int *consumed)
{
    enum { MAX_SEGMENTS = 6 };
    int max_take = token_count > MAX_SEGMENTS ? MAX_SEGMENTS : token_count;
    for (int take = max_take; take >= 1; take--) {
        char canonical[128];
        size_t off = 0;
        bool overflow = false;
        for (int i = 0; i < take; i++) {
            if (i) {
                if (off + 1 >= sizeof(canonical)) { overflow = true; break; }
                canonical[off++] = '.';
            }
            size_t tlen = strlen(tokens[i]);
            if (off + tlen + 1 >= sizeof(canonical)) { overflow = true; break; }
            memcpy(canonical + off, tokens[i], tlen);
            off += tlen;
        }
        if (overflow || off == 0) continue;
        canonical[off] = '\0';
        const sk_cli_command_t *cmd = sk_cli_lookup(canonical);
        if (cmd) {
            *consumed = take;
            return cmd;
        }
    }
    return NULL;
}

// -- Dispatcher -------------------------------------------------------------

// Returns true if the auth gate fires (and an error envelope was emitted),
// in which case the caller skips invoking the handler.
static bool reject_if_unauthenticated(sk_cli_ctx_t *ctx, const sk_cli_command_t *cmd)
{
    if (!cmd->requires_auth || ctx->authenticated) return false;
    sk_cli_err(ctx, SK_ERR_NOT_AUTHENTICATED, NULL);
    return true;
}

static void dispatch_machine(char *line, sk_cli_writer_t writer, void *user, bool authenticated)
{
    sk_cli_set_mode(SK_CLI_MODE_MACHINE);

    cJSON *msg = cJSON_Parse(line);
    if (!msg) {
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf),
                 "{\"ok\":false,\"err\":\"ERR_INVALID_ARG\",\"params\":{\"reason\":\"json_parse\"}}\n");
        writer(errbuf, strlen(errbuf), user);
        return;
    }

    cJSON *cmd_node    = cJSON_GetObjectItemCaseSensitive(msg, "cmd");
    cJSON *id_node     = cJSON_GetObjectItemCaseSensitive(msg, "id");
    cJSON *args_node   = cJSON_GetObjectItemCaseSensitive(msg, "args");
    cJSON *argv_node   = cJSON_GetObjectItemCaseSensitive(msg, "argv");
    cJSON *tok_node    = cJSON_GetObjectItemCaseSensitive(msg, "confirm_token");

    // Machine mode pozisyonel argümanlar: SKAPP `argv: ["X","Y"]` array'i
    // gönderirse string pointerlarını human_argv slotuna doldur. Böylece
    // sk_cli_arg(ctx, i) / sk_cli_argc(ctx) machine mode'da da pozisyonel
    // erişim verir; positional fallback yazan handler'lar
    // (cmd_wifi_connect gibi) machine_mode kısıtı olmadan çalışır.
    // Pointerlar msg lifetime'ına bağlı — cJSON_Delete(msg) bu dispatcher
    // sonunda çağrılır, ctx çoktan handler'dan dönmüş olur.
    const char *machine_argv_buf[SK_CLI_MAX_ARGV];
    int machine_argc = 0;
    if (cJSON_IsArray(argv_node)) {
        cJSON *item;
        cJSON_ArrayForEach(item, argv_node) {
            if (machine_argc >= SK_CLI_MAX_ARGV) break;
            if (cJSON_IsString(item) && item->valuestring) {
                machine_argv_buf[machine_argc++] = item->valuestring;
            }
        }
    }

    sk_cli_ctx_t ctx = {
        .is_machine_mode = true,
        .writer          = writer,
        .writer_user     = user,
        .machine_msg     = msg,
        .machine_args    = args_node,
        .machine_id      = cJSON_IsNumber(id_node) ? id_node->valueint : -1,
        .confirm_token   = cJSON_IsString(tok_node) ? tok_node->valuestring : NULL,
        .authenticated   = authenticated,
        .human_argv      = machine_argc > 0 ? machine_argv_buf : NULL,
        .human_argc      = machine_argc,
    };

    if (!cJSON_IsString(cmd_node)) {
        sk_cli_err(&ctx, SK_ERR_MISSING_ARG, "{\"field\":\"cmd\"}");
        cJSON_Delete(msg);
        return;
    }
    ctx.command_name = cmd_node->valuestring;

    const sk_cli_command_t *cmd = sk_cli_lookup(ctx.command_name);
    if (!cmd) {
        sk_cli_err(&ctx, SK_ERR_UNKNOWN_COMMAND, NULL);
        cJSON_Delete(msg);
        return;
    }

    if (reject_if_unauthenticated(&ctx, cmd)) {
        cJSON_Delete(msg);
        return;
    }

    // Auto-issue confirm token (machine-mode counterpart of the human path).
    // SKAPP receives the token + ttl in the error envelope's params and
    // resubmits the same cmd with `"confirm_token":"<hex>"` in the next
    // envelope. UX: the user sees one "Are you sure?" dialog; SKAPP does
    // both round-trips silently behind it.
    if (cmd->critical && !ctx.confirm_token && s_confirm_issuer) {
        char     hex[33] = {0};
        uint32_t ttl     = 0;
        if (s_confirm_issuer(hex, sizeof(hex), &ttl) == ESP_OK && hex[0]) {
            char params[160];
            snprintf(params, sizeof(params),
                     "{\"confirm_token\":\"%s\",\"ttl_sec\":%lu,\"cmd\":\"%s\"}",
                     hex, (unsigned long)ttl, cmd->name);
            sk_cli_err(&ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, params);
            cJSON_Delete(msg);
            return;
        }
        // Issuer failed — fall through to legacy handler-emitted error.
    }

    sk_err_t rc = cmd->handler(&ctx);
    if (!ctx.wrote_envelope) {
        if (rc == SK_OK) sk_cli_ok(&ctx, NULL);
        else             sk_cli_err(&ctx, rc, NULL);
    }
    cJSON_Delete(msg);
}

static void dispatch_human(char *line, sk_cli_writer_t writer, void *user, bool authenticated)
{
    sk_cli_set_mode(SK_CLI_MODE_HUMAN);

    const char *tokens[SK_CLI_MAX_ARGV];
    int token_count = tokenize(line, tokens, SK_CLI_MAX_ARGV);
    if (token_count == 0) return;  // empty line

    int consumed = 0;
    const sk_cli_command_t *cmd = resolve_human(tokens, token_count, &consumed);

    sk_cli_ctx_t ctx = {
        .is_machine_mode = false,
        .writer          = writer,
        .writer_user     = user,
        .machine_id      = -1,
        .human_argv      = tokens + consumed,
        .human_argc      = token_count - consumed,
        .authenticated   = authenticated,
    };

    if (!cmd) {
        ctx.command_name = tokens[0];
        sk_cli_writef(&ctx, "error: unknown command '%s'. Type 'help' for the list.\n", tokens[0]);
        return;
    }
    ctx.command_name = cmd->name;

    // Human-mode'da --confirm-token <hex> argümanı varsa onu ctx'e geçir
    // (machine-mode top-level "confirm_token" alanının insan eşdeğeri).
    // Bu olmadan kritik komutlar (wifi.connect, ble.unpair, ...) USB
    // CLI'dan tetiklenemiyordu.
    ctx.confirm_token = sk_cli_arg_named(&ctx, "confirm-token");

    if (reject_if_unauthenticated(&ctx, cmd)) return;

    // Auto-issue confirm token: if a critical command was called without
    // one and we have a registered issuer, mint a fresh token and emit a
    // ready-to-paste retry hint. This turns the two-step confirm-token
    // flow ("get token, then call command with token") into a one-step
    // copy/paste UX while preserving the security guarantees (single-use,
    // TTL-bound). The handler is NOT invoked in this branch.
    if (cmd->critical && !ctx.confirm_token && s_confirm_issuer) {
        char     hex[33] = {0};
        uint32_t ttl     = 0;
        if (s_confirm_issuer(hex, sizeof(hex), &ttl) == ESP_OK && hex[0]) {
            // Rebuild the original line from the token array (tokenize()
            // null-terminated each token in place, so each tokens[i] is a
            // valid C string). Stops short of overflow rather than aborting.
            char retry[SK_CLI_LINE_BUF];
            size_t off = 0;
            for (int i = 0; i < token_count && off + 1 < sizeof(retry); i++) {
                size_t tlen = strlen(tokens[i]);
                size_t need = (i > 0 ? 1 : 0) + tlen;
                if (off + need >= sizeof(retry)) break;
                if (i > 0) retry[off++] = ' ';
                memcpy(retry + off, tokens[i], tlen);
                off += tlen;
            }
            retry[off] = '\0';
            sk_cli_writef(&ctx,
                "error: ERR_CONFIRM_REQUIRED - to confirm, copy/paste this within %lus:\n",
                (unsigned long)ttl);
            sk_cli_writef(&ctx, "       %s --confirm-token %s\n", retry, hex);
            // Echo usage so the user can spot missing positional placeholders
            // (e.g. `auth passphrase clear <old>`). Without this hint the
            // rebuilt retry line carries the user's exact mistakes forward
            // and the handler later trips ERR_INVALID_ARG with no clue why.
            if (cmd->usage && cmd->usage[0]) {
                sk_cli_writef(&ctx, "       usage: %s\n", cmd->usage);
            }
            ctx.wrote_envelope = true;
            return;
        }
        // If issuer fails for any reason, fall through — the handler will
        // hit its own ERR_CONFIRM_TOKEN_REQUIRED check and respond.
    }

    sk_err_t rc = cmd->handler(&ctx);
    if (!ctx.wrote_envelope && rc != SK_OK) {
        sk_cli_err(&ctx, rc, NULL);
    }
}

static esp_err_t dispatch_common(const char *line, sk_cli_writer_t writer, void *user, bool authenticated)
{
    if (!s_ready || !line || !writer) return ESP_ERR_INVALID_ARG;

    // Copy onto a local buffer so we can tokenize (null-terminate tokens in
    // place). Skip leading whitespace.
    char buf[SK_CLI_LINE_BUF];
    while (*line && isspace((unsigned char)*line)) line++;
    size_t len = strlen(line);
    if (len == 0) return ESP_OK;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, line, len);
    buf[len] = '\0';
    // Strip trailing CR/LF.
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) buf[--len] = '\0';

    if (buf[0] == '{') {
        dispatch_machine(buf, writer, user, authenticated);
    } else {
        dispatch_human(buf, writer, user, authenticated);
    }
    return ESP_OK;
}

esp_err_t sk_cli_dispatch_line(const char *line, sk_cli_writer_t writer, void *user)
{
    return dispatch_common(line, writer, user, /*authenticated=*/false);
}

esp_err_t sk_cli_dispatch_line_authenticated(const char *line, sk_cli_writer_t writer, void *user)
{
    return dispatch_common(line, writer, user, /*authenticated=*/true);
}

// -- Built-ins --------------------------------------------------------------

static sk_err_t builtin_json_on(sk_cli_ctx_t *ctx)
{
    sk_cli_set_mode(SK_CLI_MODE_MACHINE);
    sk_cli_ok(ctx, "{\"mode\":\"machine\"}");
    return SK_OK;
}

static sk_err_t builtin_json_off(sk_cli_ctx_t *ctx)
{
    sk_cli_set_mode(SK_CLI_MODE_HUMAN);
    sk_cli_ok(ctx, "{\"mode\":\"human\"}");
    return SK_OK;
}

// Length of the namespace prefix (chars before first '.') in a command
// name. Returns 0 for namespaceless commands like "help" or "?".
static size_t namespace_len(const char *name)
{
    const char *dot = strchr(name, '.');
    return dot ? (size_t)(dot - name) : 0;
}

// Detail view for a single command — usage, full help body, critical flag.
//
// help_block can be arbitrarily long (api.endpoint.add and
// device.confirm-token both run several hundred bytes with examples).
// Use sk_cli_write for it instead of sk_cli_writef so we bypass the
// 512-byte vsnprintf scratch buffer. Same reason `usage` skips writef
// when it's just a passthrough — keeps the writer in chunked mode.
static void render_command_detail(sk_cli_ctx_t *ctx, const sk_cli_command_t *c)
{
    sk_cli_writef(ctx, "%s - %s\n", c->name, c->summary ? c->summary : "");
    if (c->usage) {
        sk_cli_write(ctx, "Usage: ", 7);
        sk_cli_write(ctx, c->usage, 0);
        sk_cli_write(ctx, "\n", 1);
    }
    if (c->help_block) {
        sk_cli_write(ctx, "\n", 1);
        sk_cli_write(ctx, c->help_block, 0);
        sk_cli_write(ctx, "\n", 1);
    }
    if (c->critical)   sk_cli_write(ctx, "\nThis is a critical command; a confirm token is required.\n", 0);
}

// All commands belonging to one namespace (e.g. "wifi" → wifi.scan, ...).
// Hidden commands are skipped; `help <name>` still resolves them directly.
static void render_topic(sk_cli_ctx_t *ctx, const char *topic)
{
    size_t tlen = strlen(topic);
    int    hits = 0;
    sk_cli_writef(ctx, "%s commands:\n", topic);
    for (int i = 0; i < s_command_count; i++) {
        const char *name = s_commands[i]->name;
        if (s_commands[i]->hidden) continue;
        if (strncmp(name, topic, tlen) == 0 && name[tlen] == '.') {
            sk_cli_writef(ctx, "  %-28s %s\n",
                          name,
                          s_commands[i]->summary ? s_commands[i]->summary : "");
            hits++;
        }
    }
    if (hits == 0) {
        sk_cli_writef(ctx, "  (no commands registered under '%s')\n", topic);
    } else {
        sk_cli_writef(ctx, "\nType 'help %s.<cmd>' for parameters and examples.\n", topic);
    }
}

// Flat list of every command — escape hatch for scripting / discovery.
// Includes hidden commands (this is the discovery view).
static void render_flat_list(sk_cli_ctx_t *ctx)
{
    sk_cli_write(ctx, "All commands:\n", 0);
    for (int i = 0; i < s_command_count; i++) {
        sk_cli_writef(ctx, "  %-28s %s%s\n",
                      s_commands[i]->name,
                      s_commands[i]->summary ? s_commands[i]->summary : "",
                      s_commands[i]->hidden ? "  [hidden]" : "");
    }
}

// True if the given namespace has at least one non-hidden command.
static bool topic_has_visible_commands(const char *topic, size_t tlen)
{
    for (int i = 0; i < s_command_count; i++) {
        if (s_commands[i]->hidden) continue;
        const char *name = s_commands[i]->name;
        if (strncmp(name, topic, tlen) == 0 && name[tlen] == '.') return true;
    }
    return false;
}

// Count visible commands belonging to a namespace.
static int topic_visible_command_count(const char *topic, size_t tlen)
{
    int n = 0;
    for (int i = 0; i < s_command_count; i++) {
        if (s_commands[i]->hidden) continue;
        const char *name = s_commands[i]->name;
        if (strncmp(name, topic, tlen) == 0 && name[tlen] == '.') n++;
    }
    return n;
}

// Render every topic that belongs to `category` (NULL matches uncategorized).
// Returns true if any line was written.
static bool render_category_topics(sk_cli_ctx_t *ctx, const char *category)
{
    bool wrote = false;
    for (int i = 0; i < s_topic_count; i++) {
        const char *cat = s_topics[i].category;
        bool match = (category == NULL)
                     ? (cat == NULL)
                     : (cat != NULL && strcmp(cat, category) == 0);
        if (!match) continue;

        const char *name = s_topics[i].name;
        size_t tlen = strlen(name);
        int count = topic_visible_command_count(name, tlen);
        if (count == 0) continue;   // skip topics with only hidden commands

        sk_cli_writef(ctx, "  %-12s %-36s (%d command%s)\n",
                      name,
                      s_topics[i].summary ? s_topics[i].summary : "",
                      count,
                      count == 1 ? "" : "s");
        wrote = true;
    }
    return wrote;
}

// Two-level overview: banner + categorized topic listing + uncategorized
// topics + namespaceless commands. Categories are emitted in the order
// they first appear in the topic table — devices control the order via
// the order of their sk_cli_register_topic() calls.
static void render_overview(sk_cli_ctx_t *ctx)
{
    sk_core_write_banner(ctx->writer, ctx->writer_user);

    // Walk topic table to discover categories in registration order;
    // dedup as we go. Hidden-only topics still contribute their category
    // to the order (cheap; harmless if the category section ends empty —
    // we skip rendering an empty section below).
    const char *seen[SK_CLI_MAX_TOPICS];
    int         seen_n = 0;
    for (int i = 0; i < s_topic_count; i++) {
        const char *cat = s_topics[i].category;
        if (!cat) continue;
        bool dup = false;
        for (int j = 0; j < seen_n; j++) {
            if (strcmp(seen[j], cat) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (seen_n < SK_CLI_MAX_TOPICS) seen[seen_n++] = cat;
    }

    for (int c = 0; c < seen_n; c++) {
        // Probe first whether the section will produce any line — keeps
        // the header out when every topic in the bucket is hidden-only.
        bool any = false;
        for (int i = 0; i < s_topic_count && !any; i++) {
            if (s_topics[i].category &&
                strcmp(s_topics[i].category, seen[c]) == 0) {
                any = topic_has_visible_commands(
                          s_topics[i].name, strlen(s_topics[i].name));
            }
        }
        if (!any) continue;

        sk_cli_writef(ctx, "\n%s\n", seen[c]);
        render_category_topics(ctx, seen[c]);
    }

    // Uncategorized topics — emitted last under a generic header.
    bool any_uncat = false;
    for (int i = 0; i < s_topic_count && !any_uncat; i++) {
        if (s_topics[i].category) continue;
        any_uncat = topic_has_visible_commands(
                       s_topics[i].name, strlen(s_topics[i].name));
    }
    if (any_uncat) {
        sk_cli_write(ctx, "\nOTHER\n", 0);
        render_category_topics(ctx, NULL);
    }

    // Namespaceless commands (e.g. `help`). Hidden ones (`json.on/off`)
    // are filtered out.
    bool any_root = false;
    for (int i = 0; i < s_command_count; i++) {
        if (s_commands[i]->hidden) continue;
        if (namespace_len(s_commands[i]->name) == 0) { any_root = true; break; }
    }
    if (any_root) {
        sk_cli_write(ctx, "\nROOT\n", 0);
        for (int i = 0; i < s_command_count; i++) {
            if (s_commands[i]->hidden) continue;
            if (namespace_len(s_commands[i]->name) != 0) continue;
            sk_cli_writef(ctx, "  %-12s %s\n",
                          s_commands[i]->name,
                          s_commands[i]->summary ? s_commands[i]->summary : "");
        }
    }

    sk_cli_write(ctx,
                 "\nType 'help <topic>' for that topic's commands,\n"
                 "     'help <command>' for one command's detail,\n"
                 "     'help all' for the full flat list (incl. hidden).\n", 0);
}

// JSON string literal yazıcı: çevreleyen tırnaklar + escape (\, ", \n, \r,
// \t, \b, \f, kontrol karakterleri). NULL ise "null" yazar (tırnaksız).
// help_block alanları gerçek newline taşıdığı için inline %s ile basamayız;
// machine-mode okuyucu (SKAPP) geçerli JSON bekler, RAW newline JSON'da
// yasak — `\n` escape sequence olmalı. summary/usage/topic için de aynı
// fonksiyon kullanılır, böylece field içinde tırnak/backslash çıkarsa
// JSON kırılmaz.
static void emit_json_string(sk_cli_ctx_t *ctx, const char *s)
{
    if (!s) {
        sk_cli_write(ctx, "null", 4);
        return;
    }
    sk_cli_write(ctx, "\"", 1);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  sk_cli_write(ctx, "\\\"", 2); break;
            case '\\': sk_cli_write(ctx, "\\\\", 2); break;
            case '\n': sk_cli_write(ctx, "\\n", 2);  break;
            case '\r': sk_cli_write(ctx, "\\r", 2);  break;
            case '\t': sk_cli_write(ctx, "\\t", 2);  break;
            case '\b': sk_cli_write(ctx, "\\b", 2);  break;
            case '\f': sk_cli_write(ctx, "\\f", 2);  break;
            default:
                if (c < 0x20) {
                    char ubuf[8];
                    int n = snprintf(ubuf, sizeof(ubuf), "\\u%04x", c);
                    if (n > 0) sk_cli_write(ctx, ubuf, (size_t)n);
                } else {
                    sk_cli_write(ctx, (const char *)&c, 1);
                }
        }
    }
    sk_cli_write(ctx, "\"", 1);
}

static sk_err_t builtin_help(sk_cli_ctx_t *ctx)
{
    const char *target = sk_cli_arg(ctx, 0);

    if (ctx->is_machine_mode) {
        // Machine mode response: commands array + topics array. SKAPP smart
        // renderer groups commands by namespace prefix and orders namespaces
        // by their topic's category — matches BF human mode's SETUP / OUTPUT
        // / SYSTEM / ROOT layout without forcing the client to hardcode the
        // mapping. Banner is human-only, never emitted here.
        //
        // Her komut için: name, summary, usage, help_block, critical, hidden.
        // usage/help_block null olabilir (eski komut tanımlarında dolu
        // değilse); SKAPP renderer null'da fallback gösterir.
        sk_cli_write(ctx, "{\"id\":", 0);
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%d", ctx->machine_id);
        sk_cli_write(ctx, idbuf, 0);
        sk_cli_write(ctx, ",\"ok\":true,\"data\":{\"commands\":[", 0);
        for (int i = 0; i < s_command_count; i++) {
            if (i) sk_cli_write(ctx, ",", 1);
            sk_cli_write(ctx, "{\"name\":", 8);
            emit_json_string(ctx, s_commands[i]->name);
            sk_cli_write(ctx, ",\"summary\":", 11);
            emit_json_string(ctx, s_commands[i]->summary);
            sk_cli_write(ctx, ",\"usage\":", 9);
            emit_json_string(ctx, s_commands[i]->usage);
            sk_cli_write(ctx, ",\"help_block\":", 14);
            emit_json_string(ctx, s_commands[i]->help_block);
            sk_cli_writef(ctx, ",\"critical\":%s,\"hidden\":%s}",
                          s_commands[i]->critical ? "true" : "false",
                          s_commands[i]->hidden   ? "true" : "false");
        }
        sk_cli_write(ctx, "],\"topics\":[", 0);
        for (int i = 0; i < s_topic_count; i++) {
            if (i) sk_cli_write(ctx, ",", 1);
            sk_cli_write(ctx, "{\"name\":", 8);
            emit_json_string(ctx, s_topics[i].name);
            sk_cli_write(ctx, ",\"summary\":", 11);
            emit_json_string(ctx, s_topics[i].summary);
            sk_cli_write(ctx, ",\"category\":", 12);
            emit_json_string(ctx, s_topics[i].category);
            sk_cli_write(ctx, "}", 1);
        }
        sk_cli_write(ctx, "]}}\n", 0);
        ctx->wrote_envelope = true;
        return SK_OK;
    }

    if (!target) {
        render_overview(ctx);
        return SK_OK;
    }

    if (strcmp(target, "all") == 0) {
        render_flat_list(ctx);
        return SK_OK;
    }

    // 1) Exact command name? → detail view.
    const sk_cli_command_t *c = sk_cli_lookup(target);
    if (c) {
        render_command_detail(ctx, c);
        return SK_OK;
    }

    // 2) Topic prefix? → list every command under <target>.*.
    size_t tlen = strlen(target);
    for (int i = 0; i < s_command_count; i++) {
        const char *name = s_commands[i]->name;
        if (strncmp(name, target, tlen) == 0 && name[tlen] == '.') {
            render_topic(ctx, target);
            return SK_OK;
        }
    }

    sk_cli_writef(ctx, "error: no command or topic '%s'. Try 'help'.\n", target);
    return SK_OK;
}
