// =====================================================================
// ls_reminder - implementation. See header.
//
// Single reminder config (subject + body + recipients) in NVS namespace
// "ls_rem". Subscribes to timer.alarm; each early-warning threshold posts
// a "fire" message to a worker task which calls ls_smtp_send. ls_smtp_send
// is synchronous, so the worker runs on its own stack (event bus handlers
// must not block). This mirrors the ls_mail_groups worker pattern, but for
// the early-warning (timer.alarm) layer instead of trigger (timer.triggered).
// =====================================================================

#include "ls_reminder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ls_smtp.h"
#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"

static const char *TAG = "ls_rem";
#define NVS_NS "ls_rem"

// Fallbacks used at send time when the user left a field empty, so a
// reminder still carries meaningful text rather than a blank mail.
#define REMINDER_DEFAULT_SUBJECT "LebensSpur reminder"
#define REMINDER_DEFAULT_BODY \
    "Your LebensSpur countdown is approaching its deadline. " \
    "Reset it to prevent the trigger actions."

typedef struct {
    bool     enabled;
    char     subject[LS_REMINDER_SUBJECT_MAX + 1];
    char     body[LS_REMINDER_BODY_MAX + 1];
    char     recipients[LS_REMINDER_RCPT_MAX][LS_REMINDER_RCPT_EMAIL_MAX + 1];
    uint8_t  recipient_count;
} reminder_cfg_t;

static reminder_cfg_t s_cfg;
static QueueHandle_t  s_fire_q = NULL;

// ---------------------------------------------------------------------
// Minimal JSON string escape: " -> \" , \ -> \\ . Control chars (< 0x20)
// are replaced with a space. Truncates if the output would exceed cap;
// output is always NUL-terminated. (Same helper shape as ls_mail_groups.)
// ---------------------------------------------------------------------
static void json_esc(char *out, size_t cap, const char *in)
{
    if (!out || cap == 0) return;
    size_t o = 0;
    for (size_t i = 0; in && in[i]; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 1 >= cap) break;
            out[o++] = ' ';
        } else {
            if (o + 1 >= cap) break;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

// Extract an integer field from a flat JSON object (device-owned payloads
// like {"index":1,"of":3,"remaining_sec":60}). Returns true on success.
static bool json_get_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) return false;
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)v;
    return true;
}

// ---------------------------------------------------------------------
// NVS persistence - flat per-field keys
// ---------------------------------------------------------------------

static void load_cfg(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t u8;
    size_t  len;

    s_cfg.enabled = (nvs_get_u8(h, "en", &u8) == ESP_OK) ? (u8 != 0) : false;

    len = sizeof(s_cfg.subject);
    nvs_get_str(h, "sb", s_cfg.subject, &len);

    len = sizeof(s_cfg.body);
    nvs_get_str(h, "bd", s_cfg.body, &len);

    if (nvs_get_u8(h, "rc", &u8) == ESP_OK) {
        s_cfg.recipient_count = (u8 <= LS_REMINDER_RCPT_MAX) ? u8 : LS_REMINDER_RCPT_MAX;
    }
    for (int r = 0; r < s_cfg.recipient_count; ++r) {
        char rk[8];
        snprintf(rk, sizeof(rk), "r%d", r);
        len = sizeof(s_cfg.recipients[r]);
        nvs_get_str(h, rk, s_cfg.recipients[r], &len);
    }
    nvs_close(h);
}

static void save_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8 (h, "en", s_cfg.enabled ? 1 : 0);
    nvs_set_str(h, "sb", s_cfg.subject);
    nvs_set_str(h, "bd", s_cfg.body);
    nvs_set_u8 (h, "rc", s_cfg.recipient_count);
    for (int r = 0; r < s_cfg.recipient_count; ++r) {
        char rk[8];
        snprintf(rk, sizeof(rk), "r%d", r);
        nvs_set_str(h, rk, s_cfg.recipients[r]);
    }
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------
// Send core (shared by the worker and reminder.test)
// ---------------------------------------------------------------------

// Send a reminder mail using a caller-provided config SNAPSHOT. Taking a
// snapshot (rather than reading the live s_cfg) means a concurrent CLI edit
// — e.g. reminder.recipient.remove on the BLE/CLI task — cannot mutate the
// recipient array underneath the multi-second SMTP send. Does NOT consider
// the `enabled` flag; the caller decides whether to fire (timer.alarm honors
// enabled; test does not).
static sk_err_t reminder_send_cfg(const reminder_cfg_t *c)
{
    if (c->recipient_count == 0)  return SK_ERR_NOT_FOUND;
    if (!ls_smtp_is_configured()) return SK_ERR_SMTP_NO_CONFIG;

    const char *rcpt_ptrs[LS_REMINDER_RCPT_MAX];
    for (int r = 0; r < c->recipient_count; ++r) {
        rcpt_ptrs[r] = c->recipients[r];
    }
    const char *subj = c->subject[0] ? c->subject : REMINDER_DEFAULT_SUBJECT;
    const char *body = c->body[0]    ? c->body    : REMINDER_DEFAULT_BODY;
    return ls_smtp_send(subj, body, rcpt_ptrs, c->recipient_count);
}

// ---------------------------------------------------------------------
// Worker: on timer.alarm, send the reminder mail
// ---------------------------------------------------------------------

typedef struct { int index; int of; } fire_msg_t;

static void on_timer_alarm(const sk_event_t *evt, void *user_ctx)
{
    (void)user_ctx;
    if (!s_fire_q) return;
    if (!s_cfg.enabled) return;   // reminders disabled — nudge nothing

    fire_msg_t m = { .index = 0, .of = 0 };
    if (evt && evt->payload_json) {
        json_get_int(evt->payload_json, "index", &m.index);
        json_get_int(evt->payload_json, "of",    &m.of);
    }
    xQueueSend(s_fire_q, &m, 0);
}

static void worker_task(void *arg)
{
    (void)arg;
    fire_msg_t m;
    for (;;) {
        if (xQueueReceive(s_fire_q, &m, portMAX_DELAY) != pdTRUE) continue;

        // Snapshot the live config: a concurrent CLI edit (recipient.remove,
        // subject, body) must not change the array under the long SMTP send.
        reminder_cfg_t snap = s_cfg;

        if (snap.recipient_count == 0) {
            ESP_LOGW(TAG, "alarm received but no recipients - skip");
            sk_event_bus_publishf("reminder.fire",
                "{\"ok\":false,\"index\":%d,\"of\":%d,\"err\":\"not_configured\"}",
                m.index, m.of);
            continue;
        }
        if (!ls_smtp_is_configured()) {
            ESP_LOGW(TAG, "alarm received but SMTP not configured - skip");
            sk_event_bus_publishf("reminder.fire",
                "{\"ok\":false,\"index\":%d,\"of\":%d,\"err\":\"smtp_not_configured\"}",
                m.index, m.of);
            continue;
        }

        sk_err_t rc = reminder_send_cfg(&snap);
        if (rc == SK_OK) {
            sk_event_bus_publishf("reminder.fire",
                "{\"ok\":true,\"index\":%d,\"of\":%d}", m.index, m.of);
        } else {
            ESP_LOGW(TAG, "reminder send failed: %s", sk_err_code_string(rc));
            sk_event_bus_publishf("reminder.fire",
                "{\"ok\":false,\"index\":%d,\"of\":%d,\"err\":\"%s\"}",
                m.index, m.of, sk_err_code_string(rc));
        }
    }
}

// ---------------------------------------------------------------------
// CLI helpers
// ---------------------------------------------------------------------

// Join argv[start..argc-1] into `out` with single spaces. Truncates if
// needed (out always NUL-terminated). Returns false if there is nothing
// to join. (Same helper shape as ls_mail_groups.)
static bool join_argv_from(sk_cli_ctx_t *ctx, int start, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    int argc = sk_cli_argc(ctx);
    if (start >= argc) return false;
    size_t o = 0;
    for (int i = start; i < argc; ++i) {
        const char *tok = sk_cli_arg(ctx, i);
        if (!tok) continue;
        if (o > 0 && o + 1 < cap) out[o++] = ' ';
        size_t tl = strlen(tok);
        size_t room = (o + 1 < cap) ? (cap - 1 - o) : 0;
        size_t copy = (tl < room) ? tl : room;
        memcpy(out + o, tok, copy);
        o += copy;
        if (o + 1 >= cap) break;
    }
    out[o] = '\0';
    return out[0] != '\0';
}

// Parse an on/off-style flag. Returns true and writes *out on success.
static bool parse_bool_flag(const char *flag, bool *out)
{
    if (!flag) return false;
    if (!strcmp(flag, "on")  || !strcmp(flag, "true")  ||
        !strcmp(flag, "yes") || !strcmp(flag, "1")) { *out = true;  return true; }
    if (!strcmp(flag, "off") || !strcmp(flag, "false") ||
        !strcmp(flag, "no")  || !strcmp(flag, "0"))  { *out = false; return true; }
    return false;
}

// ---------------------------------------------------------------------
// CLI handlers
// ---------------------------------------------------------------------

static sk_err_t cli_enable(sk_cli_ctx_t *ctx)
{
    const char *flag = sk_cli_arg(ctx, 0);
    if (!flag) flag = sk_cli_arg_after(ctx, "enabled");
    bool on;
    if (!flag || !parse_bool_flag(flag, &on)) {
        sk_cli_usage(ctx,
            "reminder enable <on|off>",
            "state: on | off | yes | no | true | false | 1 | 0",
            "reminder enable on\n"
            "reminder enable off");
        return SK_ERR_MISSING_ARG;
    }
    s_cfg.enabled = on;
    save_cfg();
    if (sk_cli_is_machine_mode(ctx)) {
        char buf[40];
        snprintf(buf, sizeof(buf), "{\"enabled\":%s}", on ? "true" : "false");
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Reminders", "%s", on ? "on" : "off");
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_subject(sk_cli_ctx_t *ctx)
{
    char subj[LS_REMINDER_SUBJECT_MAX + 1];
    const char *m_subj = sk_cli_arg_after(ctx, "subject");
    if (m_subj && m_subj[0]) {
        strncpy(subj, m_subj, sizeof(subj) - 1);
        subj[sizeof(subj) - 1] = '\0';
    } else if (!join_argv_from(ctx, 0, subj, sizeof(subj))) {
        sk_cli_usage(ctx,
            "reminder subject <text>",
            "text: 1..127 characters, multi-word allowed",
            "reminder subject LebensSpur - please reset");
        return SK_ERR_MISSING_ARG;
    }
    strncpy(s_cfg.subject, subj, sizeof(s_cfg.subject) - 1);
    s_cfg.subject[sizeof(s_cfg.subject) - 1] = '\0';
    save_cfg();
    if (sk_cli_is_machine_mode(ctx)) {
        char subj_e[LS_REMINDER_SUBJECT_MAX * 2 + 2];
        json_esc(subj_e, sizeof(subj_e), s_cfg.subject);
        char buf[320];
        snprintf(buf, sizeof(buf), "{\"subject\":\"%s\"}", subj_e);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Subject", "%s", s_cfg.subject);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_body(sk_cli_ctx_t *ctx)
{
    char body[LS_REMINDER_BODY_MAX + 1];
    const char *m_body = sk_cli_arg_after(ctx, "body");
    if (m_body && m_body[0]) {
        strncpy(body, m_body, sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';
    } else if (!join_argv_from(ctx, 0, body, sizeof(body))) {
        sk_cli_usage(ctx,
            "reminder body <text>",
            "text: 1..511 characters, multi-word allowed",
            "reminder body Please reset your LebensSpur device");
        return SK_ERR_MISSING_ARG;
    }
    strncpy(s_cfg.body, body, sizeof(s_cfg.body) - 1);
    s_cfg.body[sizeof(s_cfg.body) - 1] = '\0';
    save_cfg();
    if (sk_cli_is_machine_mode(ctx)) {
        // Large field - emit a tiny ack. Use `reminder get` to read it back.
        char buf[48];
        snprintf(buf, sizeof(buf),
            "{\"body_len\":%u}", (unsigned)strlen(s_cfg.body));
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Body", "%s", s_cfg.body);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_rcpt_add(sk_cli_ctx_t *ctx)
{
    const char *email = sk_cli_arg(ctx, 0);
    if (!email) email = sk_cli_arg_after(ctx, "email");
    if (!email || !email[0]) {
        sk_cli_usage(ctx,
            "reminder recipient add <email>",
            "email: full email address; must contain '@'",
            "reminder recipient add alice@example.com");
        return SK_ERR_MISSING_ARG;
    }
    if (!strchr(email, '@')) {
        sk_cli_usage(ctx,
            "reminder recipient add <email>",
            "email: must contain '@'",
            "reminder recipient add alice@example.com");
        return SK_ERR_INVALID_ARG;
    }
    if (s_cfg.recipient_count >= LS_REMINDER_RCPT_MAX) {
        sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"recipients full (10/10)\"}");
        return SK_ERR_BUSY;
    }
    for (int r = 0; r < s_cfg.recipient_count; ++r) {
        if (strcasecmp(s_cfg.recipients[r], email) == 0) {
            sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"reason\":\"duplicate\"}");
            return SK_ERR_INVALID_ARG;
        }
    }
    int r = s_cfg.recipient_count;
    strncpy(s_cfg.recipients[r], email, sizeof(s_cfg.recipients[r]) - 1);
    s_cfg.recipients[r][sizeof(s_cfg.recipients[r]) - 1] = '\0';
    s_cfg.recipient_count++;
    save_cfg();

    if (sk_cli_is_machine_mode(ctx)) {
        char email_e[LS_REMINDER_RCPT_EMAIL_MAX * 2 + 2];
        json_esc(email_e, sizeof(email_e), s_cfg.recipients[r]);
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"email\":\"%s\",\"count\":%u}",
            email_e, (unsigned)s_cfg.recipient_count);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Result", "Recipient added: %s", s_cfg.recipients[r]);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_rcpt_remove(sk_cli_ctx_t *ctx)
{
    const char *email = sk_cli_arg(ctx, 0);
    if (!email) email = sk_cli_arg_after(ctx, "email");
    if (!email || !email[0]) {
        sk_cli_usage(ctx,
            "reminder recipient remove <email>",
            "email: email address to remove",
            "reminder recipient remove alice@example.com");
        return SK_ERR_MISSING_ARG;
    }
    int found = -1;
    for (int r = 0; r < s_cfg.recipient_count; ++r) {
        if (strcasecmp(s_cfg.recipients[r], email) == 0) { found = r; break; }
    }
    if (found < 0) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    char removed[LS_REMINDER_RCPT_EMAIL_MAX + 1];
    strncpy(removed, s_cfg.recipients[found], sizeof(removed) - 1);
    removed[sizeof(removed) - 1] = '\0';

    for (int r = found; r < s_cfg.recipient_count - 1; ++r) {
        memcpy(s_cfg.recipients[r], s_cfg.recipients[r + 1],
               sizeof(s_cfg.recipients[r]));
    }
    memset(s_cfg.recipients[s_cfg.recipient_count - 1], 0,
           sizeof(s_cfg.recipients[0]));
    s_cfg.recipient_count--;
    save_cfg();

    if (sk_cli_is_machine_mode(ctx)) {
        char email_e[LS_REMINDER_RCPT_EMAIL_MAX * 2 + 2];
        json_esc(email_e, sizeof(email_e), removed);
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"email\":\"%s\",\"count\":%u}",
            email_e, (unsigned)s_cfg.recipient_count);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Result", "Recipient removed: %s", removed);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_get(sk_cli_ctx_t *ctx)
{
    if (sk_cli_is_machine_mode(ctx)) {
        // Worst case (after escape): subject(127*2) + body(511*2) +
        // 10 recipients x 95*2 + overhead ~ 3300 bytes. 4096 is safe.
        const size_t cap = 4096;
        char *buf = malloc(cap);
        if (!buf) {
            sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"oom\"}");
            return SK_ERR_BUSY;
        }
        char subj_e[LS_REMINDER_SUBJECT_MAX * 2 + 2];
        char body_e[LS_REMINDER_BODY_MAX * 2 + 2];
        char rcpt_e[LS_REMINDER_RCPT_EMAIL_MAX * 2 + 2];
        json_esc(subj_e, sizeof(subj_e), s_cfg.subject);
        json_esc(body_e, sizeof(body_e), s_cfg.body);

        int off = 0;
        off += snprintf(buf + off, cap - off,
            "{\"enabled\":%s,\"subject\":\"%s\",\"body\":\"%s\",\"recipients\":[",
            s_cfg.enabled ? "true" : "false", subj_e, body_e);
        for (int r = 0; r < s_cfg.recipient_count && off < (int)cap - 200; ++r) {
            json_esc(rcpt_e, sizeof(rcpt_e), s_cfg.recipients[r]);
            off += snprintf(buf + off, cap - off,
                "%s\"%s\"", r == 0 ? "" : ",", rcpt_e);
        }
        snprintf(buf + off, cap - off, "]}");
        sk_cli_ok(ctx, buf);
        free(buf);
        return SK_OK;
    }

    // Human mode - labeled rows.
    sk_cli_kvf(ctx, "Reminders", "%s", s_cfg.enabled ? "on" : "off");
    sk_cli_kvf(ctx, "Subject", "%s", s_cfg.subject[0] ? s_cfg.subject : "(empty)");
    sk_cli_kvf(ctx, "Body",    "%s", s_cfg.body[0]    ? s_cfg.body    : "(empty)");
    sk_cli_kvf(ctx, "Recipients", "(%u)", (unsigned)s_cfg.recipient_count);
    for (int r = 0; r < s_cfg.recipient_count; ++r) {
        sk_cli_writef(ctx, "    %d) %s\n", r + 1, s_cfg.recipients[r]);
    }
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_test(sk_cli_ctx_t *ctx)
{
    // Synchronous send so the caller gets an immediate ok/err verdict
    // (same UX as smtp.test). Ignores the `enabled` flag — a test should
    // fire even while reminders are paused.
    if (s_cfg.recipient_count == 0) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, "{\"reason\":\"no recipients\"}");
        return SK_ERR_NOT_FOUND;
    }
    if (!ls_smtp_is_configured()) {
        sk_cli_err(ctx, SK_ERR_SMTP_NO_CONFIG, NULL);
        return SK_ERR_SMTP_NO_CONFIG;
    }
    reminder_cfg_t snap = s_cfg;
    sk_err_t rc = reminder_send_cfg(&snap);
    if (rc != SK_OK) {
        sk_cli_err(ctx, rc, NULL);
        return rc;
    }
    if (sk_cli_is_machine_mode(ctx)) {
        char buf[64];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"recipients\":%u}", (unsigned)s_cfg.recipient_count);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Result", "Reminder sent to %u recipient(s)",
                   (unsigned)s_cfg.recipient_count);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name = "reminder.enable",
      .summary = "Enable/disable early-warning reminders: reminder enable <on|off>",
      .usage   = "reminder enable <on|off>",
      .help_block =
          "Enable or disable early-warning reminder mail.\n"
          "\n"
          "  state: on | off (also yes/no/true/false/1/0)\n"
          "\n"
          "When on, a reminder mail is sent on every timer.alarm threshold\n"
          "(the early-warning points as the deadline approaches). When off,\n"
          "the config is kept but no reminder is sent. The final\n"
          "timer.triggered mail groups are unaffected by this flag.\n"
          "\n"
          "Examples:\n"
          "  reminder enable on\n"
          "  reminder enable off",
      .handler = cli_enable },
    { .name = "reminder.subject",
      .summary = "Set reminder subject: reminder subject <text...>",
      .usage   = "reminder subject <text>",
      .help_block =
          "Set the email subject for the reminder mail.\n"
          "\n"
          "  text: 1..127 characters, multi-word allowed\n"
          "\n"
          "Examples:\n"
          "  reminder subject LebensSpur - please reset\n"
          "  reminder subject Action needed on your device",
      .handler = cli_subject },
    { .name = "reminder.body",
      .summary = "Set reminder body: reminder body <text...>",
      .usage   = "reminder body <text>",
      .help_block =
          "Set the email body for the reminder mail (plain text).\n"
          "\n"
          "  text: 1..511 characters, multi-word allowed\n"
          "\n"
          "Sent as text/plain; UTF-8 supported. Use `reminder get` to read\n"
          "the stored body back.\n"
          "\n"
          "Examples:\n"
          "  reminder body Please reset your LebensSpur device\n"
          "  reminder body Deadline approaching - take action",
      .handler = cli_body },
    { .name = "reminder.recipient.add",
      .summary = "Add reminder recipient: reminder recipient add <email> (max 10)",
      .usage   = "reminder recipient add <email>",
      .help_block =
          "Add an email recipient to the reminder list.\n"
          "\n"
          "  email: full email address; must contain '@'\n"
          "\n"
          "Up to 10 recipients. Duplicates (case-insensitive) are rejected.\n"
          "\n"
          "Examples:\n"
          "  reminder recipient add alice@example.com",
      .handler = cli_rcpt_add },
    { .name = "reminder.recipient.remove",
      .summary = "Remove reminder recipient: reminder recipient remove <email>",
      .usage   = "reminder recipient remove <email>",
      .help_block =
          "Remove a recipient from the reminder list.\n"
          "\n"
          "  email: must match an existing recipient (case-insensitive)\n"
          "\n"
          "Examples:\n"
          "  reminder recipient remove alice@example.com",
      .handler = cli_rcpt_remove },
    { .name = "reminder.get",
      .summary = "Show reminder configuration",
      .usage   = "reminder get",
      .help_block =
          "Show the reminder configuration: enabled flag, subject, body and\n"
          "every recipient.\n"
          "\n"
          "No arguments.\n"
          "\n"
          "Example:\n"
          "  reminder get",
      .handler = cli_get },
    { .name = "reminder.test",
      .summary = "Send the reminder mail once, now",
      .usage   = "reminder test",
      .help_block =
          "Send the reminder mail immediately to all configured recipients,\n"
          "regardless of the enabled flag. Requires SMTP to be configured\n"
          "(see `smtp get`) and at least one recipient.\n"
          "\n"
          "No arguments. Use this to verify delivery before relying on the\n"
          "automatic timer.alarm reminders.\n"
          "\n"
          "Example:\n"
          "  reminder test",
      .handler = cli_test },
};

// ---------------------------------------------------------------------
// Factory reset hook - wipe ls_rem NVS + zero RAM state, so a new owner
// does not inherit the previous owner's reminder list/subject/body.
// ---------------------------------------------------------------------
static void on_factory_reset(const sk_event_t *evt, void *user_ctx)
{
    (void)evt; (void)user_ctx;
    ESP_LOGW(TAG, "factory reset received - wiping ls_reminder NVS + state");

    memset(&s_cfg, 0, sizeof(s_cfg));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t ls_reminder_init(void)
{
    load_cfg();

    // Depth 2: alarm thresholds are >= 1 time-unit apart (min unit = minute)
    // while an SMTP send takes ~5-15s, so back-to-back enqueues within one
    // send window are rare. xQueueSend is non-blocking — if the queue is ever
    // full the extra alarm is intentionally dropped (a reminder is a nudge,
    // not a guaranteed-once delivery).
    s_fire_q = xQueueCreate(2, sizeof(fire_msg_t));
    if (!s_fire_q) return ESP_ERR_NO_MEM;

    // Stack 8192: the worker calls ls_smtp_send synchronously; ls_smtp's
    // mbedtls handshake + esp_tls overhead can use ~6-7 KB (same sizing
    // rationale as ls_mail_groups).
    BaseType_t ok = xTaskCreate(worker_task, "ls_rem", 8192, NULL, 4, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    int sub;
    sk_event_bus_subscribe("timer.alarm", on_timer_alarm, NULL, &sub);
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, &sub);

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); ++i) {
        sk_cli_register(&s_cmds[i]);
    }

    ESP_LOGI(TAG, "init: reminders %s, %u recipient(s)",
             s_cfg.enabled ? "on" : "off", (unsigned)s_cfg.recipient_count);
    return ESP_OK;
}
