// =====================================================================
// ls_mail_groups - implementation. See header.
//
// 10-group RAM cache + NVS persistence. On timer.triggered a "fire"
// message is posted to the worker task; the worker walks the enabled
// groups and calls ls_smtp_send on each. ls_smtp_send is synchronous, so
// the worker runs on its own stack (event bus handlers must not block).
// =====================================================================

#include "ls_mail_groups.h"

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

static const char *TAG = "ls_mg";
#define NVS_NS "ls_mg"

// Minimal JSON string escape: " -> \" , \ -> \\ . Control chars (< 0x20)
// are replaced with a space. Truncates if the output would exceed cap;
// output is always NUL-terminated. Added as part of phase 1.6 hardening.
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

static ls_mail_group_t s_groups[LS_MAIL_GROUP_MAX];
static QueueHandle_t   s_fire_q = NULL;

// ---------------------------------------------------------------------
// NVS helpers - per-group key prefix "g<i>_"
// ---------------------------------------------------------------------

static void key_for(char *out, size_t cap, int i, const char *suffix)
{
    snprintf(out, cap, "g%d_%s", i, suffix);
}

static void load_group(nvs_handle_t h, int i)
{
    char k[16];
    size_t len;
    uint8_t u8;

    key_for(k, sizeof(k), i, "used");
    if (nvs_get_u8(h, k, &u8) != ESP_OK || u8 == 0) { s_groups[i].used = false; return; }
    s_groups[i].used = true;

    key_for(k, sizeof(k), i, "en");
    s_groups[i].enabled = (nvs_get_u8(h, k, &u8) == ESP_OK) ? (u8 != 0) : true;

    key_for(k, sizeof(k), i, "nm");
    len = sizeof(s_groups[i].name);
    nvs_get_str(h, k, s_groups[i].name, &len);

    key_for(k, sizeof(k), i, "sb");
    len = sizeof(s_groups[i].subject);
    nvs_get_str(h, k, s_groups[i].subject, &len);

    key_for(k, sizeof(k), i, "bd");
    len = sizeof(s_groups[i].body);
    nvs_get_str(h, k, s_groups[i].body, &len);

    key_for(k, sizeof(k), i, "rc");
    if (nvs_get_u8(h, k, &u8) == ESP_OK) s_groups[i].recipient_count = u8;

    for (int r = 0; r < s_groups[i].recipient_count && r < LS_MAIL_GROUP_RCPT_MAX; ++r) {
        char rk[20];
        snprintf(rk, sizeof(rk), "g%d_r%d", i, r);
        len = sizeof(s_groups[i].recipients[r]);
        nvs_get_str(h, rk, s_groups[i].recipients[r], &len);
    }
}

static void save_group(int i)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k[16];

    key_for(k, sizeof(k), i, "used");
    nvs_set_u8(h, k, s_groups[i].used ? 1 : 0);

    if (s_groups[i].used) {
        key_for(k, sizeof(k), i, "en");
        nvs_set_u8(h, k, s_groups[i].enabled ? 1 : 0);
        key_for(k, sizeof(k), i, "nm");
        nvs_set_str(h, k, s_groups[i].name);
        key_for(k, sizeof(k), i, "sb");
        nvs_set_str(h, k, s_groups[i].subject);
        key_for(k, sizeof(k), i, "bd");
        nvs_set_str(h, k, s_groups[i].body);
        key_for(k, sizeof(k), i, "rc");
        nvs_set_u8(h, k, s_groups[i].recipient_count);
        for (int r = 0; r < s_groups[i].recipient_count; ++r) {
            char rk[20];
            snprintf(rk, sizeof(rk), "g%d_r%d", i, r);
            nvs_set_str(h, rk, s_groups[i].recipients[r]);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_all(void)
{
    memset(s_groups, 0, sizeof(s_groups));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < LS_MAIL_GROUP_MAX; ++i) load_group(h, i);
    nvs_close(h);
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

void ls_mail_groups_get(int id, ls_mail_group_t *out)
{
    if (!out || id < 0 || id >= LS_MAIL_GROUP_MAX) {
        if (out) memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_groups[id];
}

int ls_mail_groups_add(const char *name)
{
    for (int i = 0; i < LS_MAIL_GROUP_MAX; ++i) {
        if (!s_groups[i].used) {
            memset(&s_groups[i], 0, sizeof(s_groups[i]));
            s_groups[i].used    = true;
            s_groups[i].enabled = true;
            if (name) {
                strncpy(s_groups[i].name, name, sizeof(s_groups[i].name) - 1);
            } else {
                snprintf(s_groups[i].name, sizeof(s_groups[i].name), "group %d", i);
            }
            save_group(i);
            return i;
        }
    }
    return -1;
}

esp_err_t ls_mail_groups_delete(int id)
{
    if (id < 0 || id >= LS_MAIL_GROUP_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_groups[id].used) return ESP_ERR_NOT_FOUND;
    memset(&s_groups[id], 0, sizeof(s_groups[id]));
    save_group(id);  // .used = false is written
    return ESP_OK;
}

// ---------------------------------------------------------------------
// Worker: on timer.triggered, fire all enabled groups
// ---------------------------------------------------------------------

typedef struct { int dummy; } fire_msg_t;

static void on_timer_triggered(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    if (!s_fire_q) return;
    fire_msg_t m = {0};
    xQueueSend(s_fire_q, &m, 0);
}

static void worker_task(void *arg)
{
    (void)arg;
    fire_msg_t m;
    for (;;) {
        if (xQueueReceive(s_fire_q, &m, portMAX_DELAY) != pdTRUE) continue;

        if (!ls_smtp_is_configured()) {
            ESP_LOGW(TAG, "trigger received but SMTP not configured - skip");
            sk_event_bus_publish("mail_groups.fire",
                "{\"ok\":false,\"err\":\"smtp_not_configured\"}");
            continue;
        }

        int fired = 0, ok_count = 0;
        for (int i = 0; i < LS_MAIL_GROUP_MAX; ++i) {
            if (!s_groups[i].used) continue;
            if (!s_groups[i].enabled) continue;
            if (s_groups[i].recipient_count == 0) continue;

            const char *rcpt_ptrs[LS_MAIL_GROUP_RCPT_MAX];
            for (int r = 0; r < s_groups[i].recipient_count; ++r) {
                rcpt_ptrs[r] = s_groups[i].recipients[r];
            }

            fired++;
            sk_err_t rc = ls_smtp_send(s_groups[i].subject,
                                       s_groups[i].body,
                                       rcpt_ptrs,
                                       s_groups[i].recipient_count);
            if (rc == SK_OK) ok_count++;
            else ESP_LOGW(TAG, "group %d send failed: %s",
                          i, sk_err_code_string(rc));
        }
        sk_event_bus_publishf("mail_groups.fire",
            "{\"fired\":%d,\"ok\":%d}", fired, ok_count);
    }
}

// ---------------------------------------------------------------------
// CLI helpers
// ---------------------------------------------------------------------

// Join argv[start..argc-1] into `out` with single spaces. Truncates if
// needed (out always NUL-terminated). Returns false if start >= argc
// (no tokens to join) - the handler should emit a usage hint.
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
        if (o > 0 && o + 1 < cap) {
            out[o++] = ' ';
        }
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

// Parse an id positional. In machine mode falls back to the "id" JSON
// key. Returns id 0..LS_MAIL_GROUP_MAX-1 on success, -1 on
// missing/invalid.
static int parse_id_pos(sk_cli_ctx_t *ctx, int idx)
{
    const char *s = sk_cli_arg(ctx, idx);
    if (!s) s = sk_cli_arg_after(ctx, "id");
    if (!s) return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return -1;
    if (v < 0 || v >= LS_MAIL_GROUP_MAX) return -1;
    return (int)v;
}

static void usage_id_only(sk_cli_ctx_t *ctx, const char *usage,
                          const char *example)
{
    sk_cli_usage(ctx, usage, "id: 0..9 (group number)", example);
}

// ---------------------------------------------------------------------
// CLI handlers
// ---------------------------------------------------------------------

static sk_err_t cli_add(sk_cli_ctx_t *ctx)
{
    // Optional positional name. In machine mode fall back to "name" key.
    const char *name = sk_cli_arg(ctx, 0);
    if (!name) name = sk_cli_arg_after(ctx, "name");
    int id = ls_mail_groups_add(name);
    if (id < 0) {
        sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"groups full (10/10)\"}");
        return SK_ERR_BUSY;
    }

    if (sk_cli_is_machine_mode(ctx)) {
        char name_e[LS_MAIL_GROUP_NAME_MAX * 2 + 2];
        json_esc(name_e, sizeof(name_e), s_groups[id].name);
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"name\":\"%s\"}", id, name_e);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kv (ctx, "Result", "Group created");
        sk_cli_kvf(ctx, "id",     "%d", id);
        sk_cli_kvf(ctx, "name",   "%s", s_groups[id].name);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_delete(sk_cli_ctx_t *ctx)
{
    int id = parse_id_pos(ctx, 0);
    if (id < 0) {
        usage_id_only(ctx,
            "mail group delete <id>",
            "mail group delete 0");
        return SK_ERR_MISSING_ARG;
    }
    if (!s_groups[id].used) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    if (ls_mail_groups_delete(id) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    if (sk_cli_is_machine_mode(ctx)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"id\":%d}", id);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Result", "Group %d deleted", id);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_enable(sk_cli_ctx_t *ctx)
{
    int id = parse_id_pos(ctx, 0);
    const char *flag = sk_cli_arg(ctx, 1);
    if (!flag) flag = sk_cli_arg_after(ctx, "enabled");
    if (id < 0 || !flag) {
        sk_cli_usage(ctx,
            "mail group enable <id> <on|off>",
            "id:    0..9\n"
            "state: on | off | yes | no | true | false | 1 | 0",
            "mail group enable 0 on\n"
            "mail group enable 0 off");
        return SK_ERR_MISSING_ARG;
    }
    if (!s_groups[id].used) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    bool on;
    if (!strcmp(flag, "on")  || !strcmp(flag, "true")  ||
        !strcmp(flag, "yes") || !strcmp(flag, "1")) {
        on = true;
    } else if (!strcmp(flag, "off")  || !strcmp(flag, "false") ||
               !strcmp(flag, "no")   || !strcmp(flag, "0")) {
        on = false;
    } else {
        sk_cli_usage(ctx,
            "mail group enable <id> <on|off>",
            "state: on | off | yes | no | true | false | 1 | 0",
            "mail group enable 0 on");
        return SK_ERR_INVALID_ARG;
    }
    s_groups[id].enabled = on;
    save_group(id);
    if (sk_cli_is_machine_mode(ctx)) {
        char buf[64];
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"enabled\":%s}", id, on ? "true" : "false");
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Group",  "%d", id);
        sk_cli_kvf(ctx, "Active", "%s", on ? "yes" : "no");
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_name(sk_cli_ctx_t *ctx)
{
    int id = parse_id_pos(ctx, 0);
    if (id < 0) {
        sk_cli_usage(ctx,
            "mail group name <id> <new name>",
            "id:   0..9\n"
            "name: single or multi-word label",
            "mail group name 0 Family");
        return SK_ERR_MISSING_ARG;
    }
    if (!s_groups[id].used) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    // Name may be multi-word; join argv[1..].
    char name[LS_MAIL_GROUP_NAME_MAX + 1];
    // Fallback to "name" JSON key in machine mode.
    const char *m_name = sk_cli_arg_after(ctx, "name");
    if (m_name && m_name[0]) {
        strncpy(name, m_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    } else if (!join_argv_from(ctx, 1, name, sizeof(name))) {
        sk_cli_usage(ctx,
            "mail group name <id> <new name>",
            "name: at least 1 character",
            "mail group name 0 Family");
        return SK_ERR_MISSING_ARG;
    }
    strncpy(s_groups[id].name, name, sizeof(s_groups[id].name) - 1);
    s_groups[id].name[sizeof(s_groups[id].name) - 1] = '\0';
    save_group(id);
    if (sk_cli_is_machine_mode(ctx)) {
        char name_e[LS_MAIL_GROUP_NAME_MAX * 2 + 2];
        json_esc(name_e, sizeof(name_e), s_groups[id].name);
        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"name\":\"%s\"}", id, name_e);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Group", "%d", id);
        sk_cli_kvf(ctx, "name",  "%s", s_groups[id].name);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_subject(sk_cli_ctx_t *ctx)
{
    int id = parse_id_pos(ctx, 0);
    if (id < 0) {
        sk_cli_usage(ctx,
            "mail group subject <id> <subject>",
            "id:      0..9\n"
            "subject: multi-word allowed",
            "mail group subject 0 LebensSpur triggered");
        return SK_ERR_MISSING_ARG;
    }
    if (!s_groups[id].used) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    char subj[LS_MAIL_GROUP_SUBJECT_MAX + 1];
    const char *m_subj = sk_cli_arg_after(ctx, "subject");
    if (m_subj && m_subj[0]) {
        strncpy(subj, m_subj, sizeof(subj) - 1);
        subj[sizeof(subj) - 1] = '\0';
    } else if (!join_argv_from(ctx, 1, subj, sizeof(subj))) {
        sk_cli_usage(ctx,
            "mail group subject <id> <subject>",
            "subject: at least 1 character",
            "mail group subject 0 LebensSpur triggered");
        return SK_ERR_MISSING_ARG;
    }
    strncpy(s_groups[id].subject, subj, sizeof(s_groups[id].subject) - 1);
    s_groups[id].subject[sizeof(s_groups[id].subject) - 1] = '\0';
    save_group(id);
    if (sk_cli_is_machine_mode(ctx)) {
        char subj_e[LS_MAIL_GROUP_SUBJECT_MAX * 2 + 2];
        json_esc(subj_e, sizeof(subj_e), s_groups[id].subject);
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"subject\":\"%s\"}", id, subj_e);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Group",   "%d", id);
        sk_cli_kvf(ctx, "Subject", "%s", s_groups[id].subject);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_body(sk_cli_ctx_t *ctx)
{
    int id = parse_id_pos(ctx, 0);
    if (id < 0) {
        sk_cli_usage(ctx,
            "mail group body <id> <text>",
            "id:   0..9\n"
            "text: plain text, multi-word allowed",
            "mail group body 0 Please check your message");
        return SK_ERR_MISSING_ARG;
    }
    if (!s_groups[id].used) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    char body[LS_MAIL_GROUP_BODY_MAX + 1];
    const char *m_body = sk_cli_arg_after(ctx, "body");
    if (m_body && m_body[0]) {
        strncpy(body, m_body, sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';
    } else if (!join_argv_from(ctx, 1, body, sizeof(body))) {
        sk_cli_usage(ctx,
            "mail group body <id> <text>",
            "text: at least 1 character",
            "mail group body 0 Please check");
        return SK_ERR_MISSING_ARG;
    }
    strncpy(s_groups[id].body, body, sizeof(s_groups[id].body) - 1);
    s_groups[id].body[sizeof(s_groups[id].body) - 1] = '\0';
    save_group(id);
    if (sk_cli_is_machine_mode(ctx)) {
        // Large field - emit a tiny ack to keep the envelope small. Use
        // `mail group get` to read the full body back.
        char buf[64];
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"body_len\":%u}",
            id, (unsigned)strlen(s_groups[id].body));
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Group", "%d", id);
        sk_cli_kvf(ctx, "Body",  "%s", s_groups[id].body);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_list(sk_cli_ctx_t *ctx)
{
    if (sk_cli_is_machine_mode(ctx)) {
        // {"groups":[{"id":N,"name":"...","enabled":true,"recipients":3},...]}
        char buf[1024];
        int  off = 0;
        off += snprintf(buf + off, sizeof(buf) - off, "{\"groups\":[");
        bool first = true;
        for (int i = 0; i < LS_MAIL_GROUP_MAX; ++i) {
            if (!s_groups[i].used) continue;
            char name_e[LS_MAIL_GROUP_NAME_MAX * 2 + 2];
            json_esc(name_e, sizeof(name_e), s_groups[i].name);
            off += snprintf(buf + off, sizeof(buf) - off,
                "%s{\"id\":%d,\"name\":\"%s\",\"enabled\":%s,\"recipients\":%u}",
                first ? "" : ",",
                i, name_e,
                s_groups[i].enabled ? "true" : "false",
                (unsigned)s_groups[i].recipient_count);
            first = false;
        }
        off += snprintf(buf + off, sizeof(buf) - off, "]}");
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }

    // Human mode - labeled rows.
    int used_n = 0;
    for (int i = 0; i < LS_MAIL_GROUP_MAX; ++i) {
        if (!s_groups[i].used) continue;
        used_n++;
        sk_cli_writef(ctx, "  %d. %s (%u recipients, %s)\n",
                      i, s_groups[i].name,
                      (unsigned)s_groups[i].recipient_count,
                      s_groups[i].enabled ? "active" : "inactive");
    }
    if (used_n == 0) {
        sk_cli_kv(ctx, "Result", "no groups configured");
    }
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_get(sk_cli_ctx_t *ctx)
{
    int id = parse_id_pos(ctx, 0);
    if (id < 0) {
        usage_id_only(ctx,
            "mail group get <id>",
            "mail group get 0");
        return SK_ERR_MISSING_ARG;
    }
    if (!s_groups[id].used) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }

    if (sk_cli_is_machine_mode(ctx)) {
        // Worst case (after JSON escape): name(47*2) + subject(127*2) +
        // body(511*2) + 20 recipients x 95*2 + JSON overhead ~ 5400
        // bytes. 8192 is a safe upper bound.
        const size_t cap = 8192;
        char *buf = malloc(cap);
        if (!buf) {
            sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"oom\"}");
            return SK_ERR_BUSY;
        }
        char name_e[LS_MAIL_GROUP_NAME_MAX * 2 + 2];
        char subj_e[LS_MAIL_GROUP_SUBJECT_MAX * 2 + 2];
        char body_e[LS_MAIL_GROUP_BODY_MAX * 2 + 2];
        char rcpt_e[LS_MAIL_GROUP_RCPT_EMAIL_MAX * 2 + 2];

        json_esc(name_e, sizeof(name_e), s_groups[id].name);
        json_esc(subj_e, sizeof(subj_e), s_groups[id].subject);
        json_esc(body_e, sizeof(body_e), s_groups[id].body);

        int off = 0;
        off += snprintf(buf + off, cap - off,
            "{\"id\":%d,\"name\":\"%s\",\"enabled\":%s,\"subject\":\"%s\","
            "\"body\":\"%s\",\"recipients\":[",
            id, name_e,
            s_groups[id].enabled ? "true" : "false",
            subj_e, body_e);
        for (int r = 0; r < s_groups[id].recipient_count && off < (int)cap - 200; ++r) {
            json_esc(rcpt_e, sizeof(rcpt_e), s_groups[id].recipients[r]);
            off += snprintf(buf + off, cap - off,
                "%s\"%s\"", r == 0 ? "" : ",", rcpt_e);
        }
        snprintf(buf + off, cap - off, "]}");
        sk_cli_ok(ctx, buf);
        free(buf);
        return SK_OK;
    }

    // Human mode - labeled rows.
    sk_cli_writef(ctx, "  Group %d: %s\n", id, s_groups[id].name);
    sk_cli_kvf(ctx, "Active",  "%s", s_groups[id].enabled ? "yes" : "no");
    sk_cli_kvf(ctx, "Subject", "%s", s_groups[id].subject[0] ? s_groups[id].subject : "(empty)");
    sk_cli_kvf(ctx, "Body",    "%s", s_groups[id].body[0]    ? s_groups[id].body    : "(empty)");
    sk_cli_kvf(ctx, "Recipients", "(%u)", (unsigned)s_groups[id].recipient_count);
    for (int r = 0; r < s_groups[id].recipient_count; ++r) {
        sk_cli_writef(ctx, "    %d) %s\n", r + 1, s_groups[id].recipients[r]);
    }
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_rcpt_add(sk_cli_ctx_t *ctx)
{
    int id = parse_id_pos(ctx, 0);
    const char *email = sk_cli_arg(ctx, 1);
    if (!email) email = sk_cli_arg_after(ctx, "email");
    if (id < 0 || !email || !email[0]) {
        sk_cli_usage(ctx,
            "mail group recipient add <id> <email>",
            "id:    0..9\n"
            "email: full email address; must contain '@'",
            "mail group recipient add 0 alice@example.com");
        return SK_ERR_MISSING_ARG;
    }
    if (!s_groups[id].used) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    if (!strchr(email, '@')) {
        sk_cli_usage(ctx,
            "mail group recipient add <id> <email>",
            "email: must contain '@'",
            "mail group recipient add 0 alice@example.com");
        return SK_ERR_INVALID_ARG;
    }
    if (s_groups[id].recipient_count >= LS_MAIL_GROUP_RCPT_MAX) {
        sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"recipients full\"}");
        return SK_ERR_BUSY;
    }
    // De-duplicate - if the same address is added twice the mailer would
    // emit duplicate RCPT TO entries, wasting space and hurting Gmail's
    // spam score.
    for (int r = 0; r < s_groups[id].recipient_count; ++r) {
        if (strcasecmp(s_groups[id].recipients[r], email) == 0) {
            sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"reason\":\"duplicate\"}");
            return SK_ERR_INVALID_ARG;
        }
    }
    int r = s_groups[id].recipient_count;
    strncpy(s_groups[id].recipients[r], email,
            sizeof(s_groups[id].recipients[r]) - 1);
    s_groups[id].recipients[r][sizeof(s_groups[id].recipients[r]) - 1] = '\0';
    s_groups[id].recipient_count++;
    save_group(id);

    if (sk_cli_is_machine_mode(ctx)) {
        char email_e[LS_MAIL_GROUP_RCPT_EMAIL_MAX * 2 + 2];
        json_esc(email_e, sizeof(email_e), s_groups[id].recipients[r]);
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"email\":\"%s\",\"count\":%u}",
            id, email_e, (unsigned)s_groups[id].recipient_count);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Result", "Recipient added: %s", s_groups[id].recipients[r]);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static sk_err_t cli_rcpt_remove(sk_cli_ctx_t *ctx)
{
    int id = parse_id_pos(ctx, 0);
    const char *email = sk_cli_arg(ctx, 1);
    if (!email) email = sk_cli_arg_after(ctx, "email");
    if (id < 0 || !email || !email[0]) {
        sk_cli_usage(ctx,
            "mail group recipient remove <id> <email>",
            "id:    0..9\n"
            "email: email address to remove",
            "mail group recipient remove 0 alice@example.com");
        return SK_ERR_MISSING_ARG;
    }
    if (!s_groups[id].used) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    int found = -1;
    for (int r = 0; r < s_groups[id].recipient_count; ++r) {
        if (strcasecmp(s_groups[id].recipients[r], email) == 0) { found = r; break; }
    }
    if (found < 0) {
        sk_cli_err(ctx, SK_ERR_NOT_FOUND, NULL);
        return SK_ERR_NOT_FOUND;
    }
    char removed[LS_MAIL_GROUP_RCPT_EMAIL_MAX + 1];
    strncpy(removed, s_groups[id].recipients[found], sizeof(removed) - 1);
    removed[sizeof(removed) - 1] = '\0';

    // Shift tail
    for (int r = found; r < s_groups[id].recipient_count - 1; ++r) {
        memcpy(s_groups[id].recipients[r], s_groups[id].recipients[r + 1],
               sizeof(s_groups[id].recipients[r]));
    }
    memset(s_groups[id].recipients[s_groups[id].recipient_count - 1], 0,
           sizeof(s_groups[id].recipients[0]));
    s_groups[id].recipient_count--;
    save_group(id);

    if (sk_cli_is_machine_mode(ctx)) {
        char email_e[LS_MAIL_GROUP_RCPT_EMAIL_MAX * 2 + 2];
        json_esc(email_e, sizeof(email_e), removed);
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"id\":%d,\"email\":\"%s\",\"count\":%u}",
            id, email_e, (unsigned)s_groups[id].recipient_count);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kvf(ctx, "Result", "Recipient removed: %s", removed);
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name = "mail.group.add",
      .summary = "Create a new mail group, returns id (max 10): mail group add [name]",
      .usage   = "mail group add <name>",
      .help_block =
          "Create a new mail group.\n"
          "\n"
          "  name: optional friendly label (max 47 chars). If omitted\n"
          "        a default name \"group N\" is used.\n"
          "\n"
          "Returns the group id (0..9) - use this id with all subsequent\n"
          "mail.group.* commands. Maximum 10 groups are supported.\n"
          "\n"
          "Examples:\n"
          "  mail group add Family\n"
          "  mail group add \"Critical team\"",
      .handler = cli_add },
    { .name = "mail.group.delete",
      .summary = "Delete a mail group: mail group delete <id 0..9>",
      .usage   = "mail group delete <id>",
      .help_block =
          "Delete a mail group by id.\n"
          "\n"
          "  id: group id, 0..9\n"
          "\n"
          "All recipients, subject and body of the group are erased from\n"
          "NVS. The id may be reused by a future `mail group add`.\n"
          "\n"
          "Examples:\n"
          "  mail group delete 0",
      .handler = cli_delete },
    { .name = "mail.group.enable",
      .summary = "Enable/disable a group: mail group enable <id> <on|off>",
      .usage   = "mail group enable <id> <on|off>",
      .help_block =
          "Enable or disable a mail group.\n"
          "\n"
          "  id:    group id, 0..9\n"
          "  state: on | off (also yes/no/true/false/1/0)\n"
          "\n"
          "Disabled groups are skipped when timer.triggered fires. Use\n"
          "this to silence a group temporarily without losing its config.\n"
          "\n"
          "Examples:\n"
          "  mail group enable 0 off\n"
          "  mail group enable 1 on",
      .handler = cli_enable },
    { .name = "mail.group.name",
      .summary = "Rename a group: mail group name <id> <new name>",
      .usage   = "mail group name <id> <new name>",
      .help_block =
          "Rename a mail group. The name may contain multiple words.\n"
          "\n"
          "  id:   group id, 0..9\n"
          "  name: 1..47 characters, multi-word allowed\n"
          "\n"
          "Examples:\n"
          "  mail group name 0 Family\n"
          "  mail group name 1 On call team",
      .handler = cli_name },
    { .name = "mail.group.subject",
      .summary = "Set email subject: mail group subject <id> <text...>",
      .usage   = "mail group subject <id> <subject>",
      .help_block =
          "Set the email subject line for a group.\n"
          "\n"
          "  id:      group id, 0..9\n"
          "  subject: 1..127 characters, multi-word allowed\n"
          "\n"
          "Applied to every mail this group sends when timer.triggered\n"
          "fires.\n"
          "\n"
          "Examples:\n"
          "  mail group subject 0 LebensSpur triggered\n"
          "  mail group subject 0 Reminder from your device",
      .handler = cli_subject },
    { .name = "mail.group.body",
      .summary = "Set email body: mail group body <id> <text...>",
      .usage   = "mail group body <id> <text>",
      .help_block =
          "Set the email body for a group (plain text).\n"
          "\n"
          "  id:   group id, 0..9\n"
          "  text: 1..511 characters, multi-word allowed\n"
          "\n"
          "The body is sent as text/plain; UTF-8 is supported. Use\n"
          "`mail group get <id>` to read the stored body back.\n"
          "\n"
          "Examples:\n"
          "  mail group body 0 Please check your message\n"
          "  mail group body 1 Timer expired - take action",
      .handler = cli_body },
    { .name = "mail.group.list",
      .summary = "List all configured groups",
      .usage   = "mail group list",
      .help_block =
          "Show all configured groups with their id, name, enabled flag\n"
          "and recipient count.\n"
          "\n"
          "No arguments. Use `mail group get <id>` for full detail of a\n"
          "single group.\n"
          "\n"
          "Example:\n"
          "  mail group list",
      .handler = cli_list },
    { .name = "mail.group.get",
      .summary = "Show a single group's full detail",
      .usage   = "mail group get <id>",
      .help_block =
          "Show the full detail of one group: name, enabled flag, subject,\n"
          "body and every recipient.\n"
          "\n"
          "  id: group id, 0..9\n"
          "\n"
          "Example:\n"
          "  mail group get 0",
      .handler = cli_get },
    { .name = "mail.group.recipient.add",
      .summary = "Add recipient: mail group recipient add <id> <email> (max 20)",
      .usage   = "mail group recipient add <id> <email>",
      .help_block =
          "Add an email recipient to a group.\n"
          "\n"
          "  id:    group id, 0..9\n"
          "  email: full email address; must contain '@'\n"
          "\n"
          "A group can hold up to 20 recipients. Duplicates\n"
          "(case-insensitive) are rejected.\n"
          "\n"
          "Examples:\n"
          "  mail group recipient add 0 alice@example.com\n"
          "  mail group recipient add 0 bob@example.com",
      .handler = cli_rcpt_add },
    { .name = "mail.group.recipient.remove",
      .summary = "Remove recipient: mail group recipient remove <id> <email>",
      .usage   = "mail group recipient remove <id> <email>",
      .help_block =
          "Remove a recipient from a group.\n"
          "\n"
          "  id:    group id, 0..9\n"
          "  email: must match an existing recipient (case-insensitive)\n"
          "\n"
          "Examples:\n"
          "  mail group recipient remove 0 alice@example.com",
      .handler = cli_rcpt_remove },
};

// ---------------------------------------------------------------------
// Factory reset hook
// ---------------------------------------------------------------------

// On device.factory-reset.requested: wipe ls_mg NVS + zero all 10 group
// slots in RAM. Pairing-completeness fix: previously the recipient
// addresses + subjects + bodies persisted, so a new owner inherited the
// previous owner's mailing lists.
static void on_factory_reset(const sk_event_t *evt, void *user_ctx)
{
    (void)evt; (void)user_ctx;
    ESP_LOGW(TAG, "factory reset received — wiping ls_mail_groups NVS + state");

    // 1) Clear all 10 group slots.
    memset(s_groups, 0, sizeof(s_groups));

    // 2) Wipe NVS namespace.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t ls_mail_groups_init(void)
{
    load_all();

    s_fire_q = xQueueCreate(2, sizeof(fire_msg_t));
    if (!s_fire_q) return ESP_ERR_NO_MEM;

    // Stack 5120 -> 8192: the worker calls ls_smtp_send synchronously;
    // ls_smtp's mbedtls handshake + esp_tls overhead can use ~6-7 KB.
    // 5120 was marginal and could overflow silently. 8192 is safe.
    BaseType_t ok = xTaskCreate(worker_task, "ls_mg", 8192, NULL, 4, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    int sub;
    sk_event_bus_subscribe("timer.triggered", on_timer_triggered, NULL, &sub);

    // Factory reset hook — wipe NVS + zero all 10 group slots.
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, &sub);

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); ++i) {
        sk_cli_register(&s_cmds[i]);
    }

    int used = 0;
    for (int i = 0; i < LS_MAIL_GROUP_MAX; ++i) if (s_groups[i].used) used++;
    ESP_LOGI(TAG, "init: %d/%d groups loaded", used, LS_MAIL_GROUP_MAX);
    return ESP_OK;
}
