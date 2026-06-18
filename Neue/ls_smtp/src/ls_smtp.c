// =====================================================================
// ls_smtp - implementation. See header.
//
// SMTPS over esp-tls (port 465). Flow:
//   connect -> recv 220 -> EHLO -> AUTH LOGIN -> MAIL FROM -> RCPT TO (x N)
//   -> DATA -> \r\n.\r\n -> QUIT -> disconnect
//
// Server reply codes: first 3 characters of the first line (e.g. "250",
// "334", "235", "354"). Multi-line replies use "<code>-..." continuation;
// the final line uses "<code> ..." (space separator). This simple client
// supports multi-line replies but only inspects the final line.
// =====================================================================

#include "ls_smtp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"

static const char *TAG = "ls_smtp";

#define NVS_NS         "ls_smtp"
#define NVS_KEY_HOST   "host"
#define NVS_KEY_PORT   "port"
#define NVS_KEY_SENDER "sender"
#define NVS_KEY_KEY    "key"

#define DEFAULT_PORT   465
#define IO_BUF         512
#define RECV_TIMEOUT_MS 10000

static ls_smtp_config_t s_cfg;

// ---------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------

static void cfg_clear(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.port = DEFAULT_PORT;
}

static void nvs_load(void)
{
    cfg_clear();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len;
    len = sizeof(s_cfg.host);   nvs_get_str(h, NVS_KEY_HOST,   s_cfg.host,   &len);
    len = sizeof(s_cfg.sender); nvs_get_str(h, NVS_KEY_SENDER, s_cfg.sender, &len);
    len = sizeof(s_cfg.api_key);nvs_get_str(h, NVS_KEY_KEY,    s_cfg.api_key,&len);
    uint16_t p;
    if (nvs_get_u16(h, NVS_KEY_PORT, &p) == ESP_OK) s_cfg.port = p;

    nvs_close(h);
}

static void nvs_save_host(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_HOST, s_cfg.host);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_port(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, NVS_KEY_PORT, s_cfg.port);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_sender(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_SENDER, s_cfg.sender);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_key(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_KEY, s_cfg.api_key);
    nvs_commit(h);
    nvs_close(h);
}

// Write all four fields in a single NVS transaction (one commit). Used by
// smtp.save so a reboot can't land between the per-field setters and leave
// a partial config — the SKAPP pushes host+port+sender+key as one unit.
static void nvs_save_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_HOST,   s_cfg.host);
    nvs_set_u16(h, NVS_KEY_PORT,   s_cfg.port);
    nvs_set_str(h, NVS_KEY_SENDER, s_cfg.sender);
    nvs_set_str(h, NVS_KEY_KEY,    s_cfg.api_key);
    nvs_commit(h);
    nvs_close(h);
}

bool ls_smtp_is_configured(void)
{
    return s_cfg.host[0] != '\0' && s_cfg.sender[0] != '\0';
}

void ls_smtp_config(ls_smtp_config_t *out)
{
    if (!out) return;
    *out = s_cfg;
}

// ---------------------------------------------------------------------
// Wire helpers
// ---------------------------------------------------------------------

static int tls_write_all(esp_tls_t *tls, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = esp_tls_conn_write(tls, buf + sent, len - sent);
        if (n < 0) return -1;
        if (n == 0) continue;
        sent += (size_t)n;
    }
    return 0;
}

// Read the response and return the first 3-digit code of the final line
// (int). Returns -1 on error.
static int tls_read_response(esp_tls_t *tls, char *buf, size_t buf_size)
{
    size_t total = 0;
    int    last_code = -1;
    for (;;) {
        if (total >= buf_size - 1) break;
        ssize_t n = esp_tls_conn_read(tls, buf + total, buf_size - 1 - total);
        // Phase 1.6 fix: esp_tls may surface negative MBEDTLS_ERR_* codes
        // (large negative integers). (int)n casts may overflow depending on
        // arch; the only correct treatment is -1.
        if (n < 0)  return -1;
        if (n == 0) break;
        total += (size_t)n;
        buf[total] = '\0';

        // Find the final line via "\r\n<code> ".
        // In multi-line responses non-final lines end with "-", the final
        // line ends with " " (space). Walk the buffer line by line and
        // grab the last complete line.
        char *line_start = buf;
        char *cr;
        char *last_complete_line = NULL;
        while ((cr = strstr(line_start, "\r\n")) != NULL) {
            last_complete_line = line_start;
            line_start = cr + 2;
        }
        // If the last complete line has a space at column 4, the transfer
        // is done.
        if (last_complete_line && strlen(last_complete_line) >= 4 &&
            last_complete_line[3] == ' ') {
            // Extract the code of the final line.
            char code[4] = {
                last_complete_line[0], last_complete_line[1],
                last_complete_line[2], 0
            };
            last_code = atoi(code);
            return last_code;
        }
    }
    return last_code;
}

static int tls_cmd(esp_tls_t *tls, const char *cmd, char *resp, size_t resp_size)
{
    if (tls_write_all(tls, cmd, strlen(cmd)) < 0) return -1;
    return tls_read_response(tls, resp, resp_size);
}

// ---------------------------------------------------------------------
// SMTP transaction
// ---------------------------------------------------------------------

static sk_err_t do_send(const char *subject,
                        const char *body,
                        const char *const *recipients,
                        int n_recipients)
{
    if (!ls_smtp_is_configured())  return SK_ERR_SMTP_NO_CONFIG;
    if (n_recipients <= 0)         return SK_ERR_INVALID_ARG;
    if (n_recipients > LS_SMTP_MAX_RCPT) n_recipients = LS_SMTP_MAX_RCPT;

    // Phase 1.6 hardening: ESP-IDF cert bundle (includes the popular CA
    // roots), hostname verification on. Enabled via sdkconfig
    // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
    // (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y) - Telegram, Gmail
    // SMTP, SendGrid etc. validate out-of-the-box.
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = RECV_TIMEOUT_MS,
    };

    esp_tls_t *tls = esp_tls_init();
    if (!tls) return SK_ERR_SMTP_CONNECT;

    sk_err_t rc = SK_OK;
    char rbuf[IO_BUF];
    char wbuf[IO_BUF];

    if (esp_tls_conn_new_sync(s_cfg.host, (int)strlen(s_cfg.host),
                              s_cfg.port, &cfg, tls) != 1) {
        rc = SK_ERR_SMTP_TLS;
        goto out;
    }

    // 1) Banner
    if (tls_read_response(tls, rbuf, sizeof(rbuf)) != 220) { rc = SK_ERR_SMTP_CONNECT; goto out; }

    // 2) EHLO
    snprintf(wbuf, sizeof(wbuf), "EHLO lebensspur\r\n");
    if (tls_cmd(tls, wbuf, rbuf, sizeof(rbuf)) != 250) { rc = SK_ERR_SMTP_CONNECT; goto out; }

    // 3) AUTH LOGIN
    if (tls_cmd(tls, "AUTH LOGIN\r\n", rbuf, sizeof(rbuf)) != 334) { rc = SK_ERR_SMTP_AUTH; goto out; }

    // 3a) base64(user)
    unsigned char b64[256];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
            (const unsigned char *)s_cfg.sender, strlen(s_cfg.sender)) != 0) {
        rc = SK_ERR_SMTP_AUTH; goto out;
    }
    snprintf(wbuf, sizeof(wbuf), "%.*s\r\n", (int)b64_len, (char *)b64);
    if (tls_cmd(tls, wbuf, rbuf, sizeof(rbuf)) != 334) { rc = SK_ERR_SMTP_AUTH; goto out; }

    // 3b) base64(password / api_key)
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
            (const unsigned char *)s_cfg.api_key, strlen(s_cfg.api_key)) != 0) {
        rc = SK_ERR_SMTP_AUTH; goto out;
    }
    snprintf(wbuf, sizeof(wbuf), "%.*s\r\n", (int)b64_len, (char *)b64);
    if (tls_cmd(tls, wbuf, rbuf, sizeof(rbuf)) != 235) { rc = SK_ERR_SMTP_AUTH; goto out; }

    // 4) MAIL FROM
    snprintf(wbuf, sizeof(wbuf), "MAIL FROM:<%s>\r\n", s_cfg.sender);
    if (tls_cmd(tls, wbuf, rbuf, sizeof(rbuf)) != 250) { rc = SK_ERR_SMTP_CONNECT; goto out; }

    // 5) RCPT TO (per recipient). At least one must be accepted; if all
    // are rejected we bail out early with a meaningful error (instead of
    // the previous DATA->503 outcome).
    int rcpt_ok = 0;
    for (int i = 0; i < n_recipients; ++i) {
        if (!recipients[i] || !recipients[i][0]) continue;
        snprintf(wbuf, sizeof(wbuf), "RCPT TO:<%s>\r\n", recipients[i]);
        int code = tls_cmd(tls, wbuf, rbuf, sizeof(rbuf));
        if (code == 250 || code == 251) {
            rcpt_ok++;
        } else {
            ESP_LOGW(TAG, "RCPT TO <%s> rejected: %d", recipients[i], code);
        }
    }
    if (rcpt_ok == 0) {
        ESP_LOGE(TAG, "All recipients rejected");
        rc = SK_ERR_SMTP_CONNECT;
        goto out;
    }

    // 6) DATA
    if (tls_cmd(tls, "DATA\r\n", rbuf, sizeof(rbuf)) != 354) { rc = SK_ERR_SMTP_CONNECT; goto out; }

    // 6a) Header: To: lists the first 5 recipients (visible To: list).
    // Remaining recipients are still delivered via RCPT TO but not shown
    // in the header (bcc semantics).
    {
        char header[IO_BUF];
        int  off = 0;
        off += snprintf(header + off, sizeof(header) - off,
                        "From: <%s>\r\n", s_cfg.sender);
        off += snprintf(header + off, sizeof(header) - off, "To: ");
        int shown = 0;
        for (int i = 0; i < n_recipients && shown < 5; ++i) {
            if (!recipients[i] || !recipients[i][0]) continue;
            if (shown > 0) off += snprintf(header + off, sizeof(header) - off, ", ");
            off += snprintf(header + off, sizeof(header) - off, "<%s>", recipients[i]);
            shown++;
        }
        off += snprintf(header + off, sizeof(header) - off, "\r\n");
        off += snprintf(header + off, sizeof(header) - off,
                        "Subject: %s\r\n", subject ? subject : "");
        off += snprintf(header + off, sizeof(header) - off,
                        "Content-Type: text/plain; charset=UTF-8\r\n\r\n");
        if (tls_write_all(tls, header, (size_t)off) < 0) { rc = SK_ERR_SMTP_CONNECT; goto out; }
    }

    // 6b) Body
    if (body && body[0]) {
        if (tls_write_all(tls, body, strlen(body)) < 0) { rc = SK_ERR_SMTP_CONNECT; goto out; }
    }

    // 6c) DATA terminator
    if (tls_cmd(tls, "\r\n.\r\n", rbuf, sizeof(rbuf)) != 250) {
        rc = SK_ERR_SMTP_CONNECT; goto out;
    }

    // 7) QUIT
    tls_write_all(tls, "QUIT\r\n", 6);
    // 221 reply; ignored - the connection is being torn down anyway.

out:
    esp_tls_conn_destroy(tls);
    return rc;
}

sk_err_t ls_smtp_send(const char *subject,
                      const char *body,
                      const char *const *recipients,
                      int n_recipients)
{
    char to_summary[64] = {0};
    if (n_recipients > 0 && recipients && recipients[0]) {
        snprintf(to_summary, sizeof(to_summary), "%s%s",
                 recipients[0],
                 (n_recipients > 1) ? " +N" : "");
    }
    sk_event_bus_publishf("smtp.send.start",
        "{\"to\":\"%s\",\"subject\":\"%s\"}",
        to_summary, subject ? subject : "");

    sk_err_t rc = do_send(subject, body, recipients, n_recipients);

    if (rc == SK_OK) {
        sk_event_bus_publish("smtp.send.end", "{\"ok\":true}");
    } else {
        sk_event_bus_publishf("smtp.send.end",
            "{\"ok\":false,\"err\":\"%s\"}", sk_err_code_string(rc));
    }
    return rc;
}

// ---------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------

static sk_err_t cli_host(sk_cli_ctx_t *ctx)
{
    const char *v = sk_cli_arg_after(ctx, "host");
    if (!v) v = sk_cli_arg(ctx, 0);
    if (!v || !v[0]) {
        sk_cli_usage(ctx,
            "smtp host <hostname>",
            "hostname: SMTPS server FQDN (port 465)",
            "smtp host smtp.gmail.com");
        return SK_ERR_MISSING_ARG;
    }
    strncpy(s_cfg.host, v, sizeof(s_cfg.host) - 1);
    s_cfg.host[sizeof(s_cfg.host) - 1] = '\0';
    nvs_save_host();
    char data[160];
    snprintf(data, sizeof(data), "{\"host\":\"%s\"}", s_cfg.host);
    sk_cli_ok(ctx, data);
    return SK_OK;
}

static sk_err_t cli_port(sk_cli_ctx_t *ctx)
{
    long port_l = 0;
    bool have = sk_cli_arg_after_long(ctx, "port", &port_l);
    if (!have) {
        const char *v = sk_cli_arg(ctx, 0);
        if (v && v[0]) { port_l = atol(v); have = true; }
    }
    if (!have) {
        sk_cli_usage(ctx,
            "smtp port <N>",
            "N: 1..65535 (SMTPS default 465)",
            "smtp port 465");
        return SK_ERR_MISSING_ARG;
    }
    if (port_l < 1 || port_l > 65535) {
        sk_cli_usage(ctx,
            "smtp port <N>",
            "N: 1..65535",
            "smtp port 465");
        return SK_ERR_INVALID_ARG;
    }
    s_cfg.port = (uint16_t)port_l;
    nvs_save_port();
    char data[64];
    snprintf(data, sizeof(data), "{\"port\":%u}", (unsigned)s_cfg.port);
    sk_cli_ok(ctx, data);
    return SK_OK;
}

static sk_err_t cli_sender(sk_cli_ctx_t *ctx)
{
    const char *v = sk_cli_arg_after(ctx, "sender");
    if (!v) v = sk_cli_arg(ctx, 0);
    if (!v || !v[0]) {
        sk_cli_usage(ctx,
            "smtp sender <email>",
            "email: sender address (also SMTP AUTH user)",
            "smtp sender me@gmail.com");
        return SK_ERR_MISSING_ARG;
    }
    if (!strchr(v, '@')) {
        sk_cli_usage(ctx,
            "smtp sender <email>",
            "email: must contain '@'",
            "smtp sender me@gmail.com");
        return SK_ERR_INVALID_ARG;
    }
    strncpy(s_cfg.sender, v, sizeof(s_cfg.sender) - 1);
    s_cfg.sender[sizeof(s_cfg.sender) - 1] = '\0';
    nvs_save_sender();
    char data[200];
    snprintf(data, sizeof(data), "{\"sender\":\"%s\"}", s_cfg.sender);
    sk_cli_ok(ctx, data);
    return SK_OK;
}

static sk_err_t cli_key(sk_cli_ctx_t *ctx)
{
    const char *v = sk_cli_arg_after(ctx, "key");
    if (!v) v = sk_cli_arg(ctx, 0);
    if (!v || !v[0]) {
        sk_cli_usage(ctx,
            "smtp key <api_key>",
            "api_key: SMTP AUTH password (Gmail App Password, SendGrid token, ...)",
            "smtp key abcdefghijklmnop");
        return SK_ERR_MISSING_ARG;
    }
    strncpy(s_cfg.api_key, v, sizeof(s_cfg.api_key) - 1);
    s_cfg.api_key[sizeof(s_cfg.api_key) - 1] = '\0';
    nvs_save_key();
    char data[64];
    snprintf(data, sizeof(data),
        "{\"key\":\"(set, %u chars)\"}", (unsigned)strlen(s_cfg.api_key));
    sk_cli_ok(ctx, data);
    return SK_OK;
}

static sk_err_t cli_get(sk_cli_ctx_t *ctx)
{
    char masked[40] = {0};
    if (s_cfg.api_key[0]) {
        size_t L = strlen(s_cfg.api_key);
        snprintf(masked, sizeof(masked), "(set, %u chars)", (unsigned)L);
    }

    if (sk_cli_is_machine_mode(ctx)) {
        char buf[400];
        snprintf(buf, sizeof(buf),
            "{\"host\":\"%s\",\"port\":%u,\"sender\":\"%s\",\"api_key\":\"%s\"}",
            s_cfg.host, (unsigned)s_cfg.port, s_cfg.sender, masked);
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }

    // Human mode - labeled rows.
    sk_cli_kvf(ctx, "Server", "%s", s_cfg.host[0]   ? s_cfg.host   : "(not set)");
    sk_cli_kvf(ctx, "Port",   "%u", (unsigned)s_cfg.port);
    sk_cli_kvf(ctx, "Sender", "%s", s_cfg.sender[0] ? s_cfg.sender : "(not set)");
    if (s_cfg.api_key[0]) {
        sk_cli_kvf(ctx, "API key", "(set, %u chars)",
                   (unsigned)strlen(s_cfg.api_key));
    } else {
        sk_cli_kv (ctx, "API key", "(not set)");
    }
    return SK_OK;
}

// The SMTPS handshake + send can take 5-15 seconds; to keep the CLI
// transport task (USB CLI / TCP client) unblocked we dispatch to a
// separate task. The outcome is published on the event bus
// (smtp.send.end) - this command itself just returns
// "{\"started\":true}". The original LebensSpur smtp.test handler used
// the same model (ls_mail_cmds.c).
static void smtp_test_task(void *arg)
{
    (void)arg;
    const char *rcpt[1] = { s_cfg.sender };
    (void)ls_smtp_send("LebensSpur SMTP test",
                       "This is an SMTP configuration test from the LebensSpur device.\r\n",
                       rcpt, 1);
    vTaskDelete(NULL);
}

static sk_err_t cli_test(sk_cli_ctx_t *ctx)
{
    if (!ls_smtp_is_configured()) {
        sk_cli_err(ctx, SK_ERR_SMTP_NO_CONFIG, NULL);
        return SK_ERR_SMTP_NO_CONFIG;
    }
    // 8192 byte stack: safe for esp_tls + mbedtls handshake (the original
    // LebensSpur used 10240; modern esp_tls is slightly leaner).
    BaseType_t ok = xTaskCreate(smtp_test_task, "smtp_test",
                                8192, NULL, 4, NULL);
    if (ok != pdPASS) {
        sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"oom\"}");
        return SK_ERR_BUSY;
    }

    if (sk_cli_is_machine_mode(ctx)) {
        sk_cli_ok(ctx, "{\"started\":true}");
    } else {
        sk_cli_kv(ctx, "Result", "test mail queued");
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

// Atomic multi-field save. SKAPP pushes host+port+sender+key as one unit;
// all four land in a single NVS commit (nvs_save_all) so a reboot can't
// leave a partial config. Any subset is accepted — omitted fields keep
// their current value. Validation runs before any mutation.
static sk_err_t cli_save(sk_cli_ctx_t *ctx)
{
    const char *host   = sk_cli_arg_after(ctx, "host");
    const char *sender = sk_cli_arg_after(ctx, "sender");
    const char *key    = sk_cli_arg_after(ctx, "key");
    long port_l = 0;
    bool have_port = sk_cli_arg_after_long(ctx, "port", &port_l);

    if (!host && !sender && !key && !have_port) {
        sk_cli_usage(ctx,
            "smtp save [host <h>] [port <N>] [sender <e>] [key <k>]",
            "All fields written in one atomic NVS commit. Any subset is\n"
            "allowed; omitted fields keep their current value.",
            "smtp save host smtp.gmail.com port 465 sender me@gmail.com key <app_pw>");
        return SK_ERR_MISSING_ARG;
    }
    // Validate before mutating s_cfg, so a bad field leaves config untouched.
    if (have_port && (port_l < 1 || port_l > 65535)) {
        sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"port\"}");
        return SK_ERR_INVALID_ARG;
    }
    if (sender && sender[0] && !strchr(sender, '@')) {
        sk_cli_err(ctx, SK_ERR_INVALID_EMAIL, "{\"field\":\"sender\"}");
        return SK_ERR_INVALID_EMAIL;
    }

    if (host) {
        strncpy(s_cfg.host, host, sizeof(s_cfg.host) - 1);
        s_cfg.host[sizeof(s_cfg.host) - 1] = '\0';
    }
    if (have_port) s_cfg.port = (uint16_t)port_l;
    if (sender) {
        strncpy(s_cfg.sender, sender, sizeof(s_cfg.sender) - 1);
        s_cfg.sender[sizeof(s_cfg.sender) - 1] = '\0';
    }
    if (key) {
        strncpy(s_cfg.api_key, key, sizeof(s_cfg.api_key) - 1);
        s_cfg.api_key[sizeof(s_cfg.api_key) - 1] = '\0';
    }
    nvs_save_all();

    if (sk_cli_is_machine_mode(ctx)) {
        char masked[40] = {0};
        if (s_cfg.api_key[0]) {
            snprintf(masked, sizeof(masked),
                     "(set, %u chars)", (unsigned)strlen(s_cfg.api_key));
        }
        char buf[400];
        snprintf(buf, sizeof(buf),
            "{\"host\":\"%s\",\"port\":%u,\"sender\":\"%s\",\"api_key\":\"%s\"}",
            s_cfg.host, (unsigned)s_cfg.port, s_cfg.sender, masked);
        sk_cli_ok(ctx, buf);
    } else {
        sk_cli_kv(ctx, "Result", "SMTP config saved");
        sk_cli_ok(ctx, NULL);
    }
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name = "smtp.host",
      .summary = "Set SMTPS server hostname (e.g. smtp.gmail.com)",
      .usage   = "smtp host <hostname>",
      .help_block =
          "Set the SMTPS server hostname.\n"
          "\n"
          "  hostname: SMTPS server FQDN.\n"
          "            Examples: smtp.gmail.com, smtp.sendgrid.net,\n"
          "            smtp.mailgun.org\n"
          "\n"
          "Use the TLS-from-start port 465 endpoint (configured via\n"
          "`smtp port`). STARTTLS (port 587) is NOT supported in this\n"
          "phase; use the SSL-only port.\n"
          "\n"
          "Examples:\n"
          "  smtp host smtp.gmail.com\n"
          "  smtp host smtp.sendgrid.net",
      .handler = cli_host },
    { .name = "smtp.port",
      .summary = "Set SMTPS port (1..65535)",
      .usage   = "smtp port <N>",
      .help_block =
          "Set the SMTPS TCP port.\n"
          "\n"
          "  N: 1..65535, typically 465 (SMTPS / TLS-from-start).\n"
          "\n"
          "This firmware does NOT support STARTTLS (port 587). For Gmail,\n"
          "SendGrid, Mailgun and Outlook use the SSL endpoint on 465.\n"
          "\n"
          "Examples:\n"
          "  smtp port 465",
      .handler = cli_port },
    { .name = "smtp.sender",
      .summary = "Set sender email, also SMTP AUTH user (e.g. me@gmail.com)",
      .usage   = "smtp sender <email>",
      .help_block =
          "Set the sender email address (also used as AUTH LOGIN user).\n"
          "\n"
          "  email: full email address; must contain '@'.\n"
          "\n"
          "For Gmail this MUST be the account that owns the App Password\n"
          "configured via `smtp key`. The address appears in the From:\n"
          "header of outgoing mail.\n"
          "\n"
          "Examples:\n"
          "  smtp sender me@gmail.com\n"
          "  smtp sender alerts@example.com",
      .handler = cli_sender },
    { .name = "smtp.key",
      .summary = "Set SMTP AUTH password (Gmail App Password / API key)",
      .usage   = "smtp key <api_key>",
      .help_block =
          "Set the SMTP AUTH password (or app-specific password).\n"
          "\n"
          "  key: provider-specific secret.\n"
          "\n"
          "For Gmail: enable 2-factor auth on the Google account, then\n"
          "create an App Password at https://myaccount.google.com/apppasswords\n"
          "and paste it here (16 chars, no spaces).\n"
          "For SendGrid / Mailgun: use the API key from their dashboard.\n"
          "\n"
          "The value is stored in NVS (not encrypted in this phase) and\n"
          "masked in `smtp get` output (shown as \"(set, N chars)\").\n"
          "\n"
          "Examples:\n"
          "  smtp key abcdefghijklmnop\n"
          "  smtp key SG.xxxxxxxxxxxxxxxxxxxx",
      .handler = cli_key },
    { .name = "smtp.get",
      .summary = "Show SMTP configuration (api_key masked)",
      .usage   = "smtp get",
      .help_block =
          "Show the SMTP configuration. API key is masked (shows length\n"
          "only).\n"
          "\n"
          "No arguments.\n"
          "\n"
          "Example:\n"
          "  smtp get",
      .handler = cli_get },
    { .name = "smtp.save",
      .summary = "Save host+port+sender+key atomically (one NVS commit)",
      .usage   = "smtp save [host <h>] [port <N>] [sender <e>] [key <k>]",
      .help_block =
          "Save the SMTP configuration in a single atomic NVS commit.\n"
          "\n"
          "  host:   SMTPS server FQDN (port 465)\n"
          "  port:   1..65535 (typically 465)\n"
          "  sender: sender email (also AUTH user); must contain '@'\n"
          "  key:    SMTP AUTH password / App Password / API key\n"
          "\n"
          "Any subset is accepted; omitted fields keep their current value.\n"
          "Unlike the per-field setters (smtp host/port/sender/key) this\n"
          "writes everything in ONE commit, so a reboot can't leave a\n"
          "partial config. SKAPP uses this to push the whole form at once.\n"
          "\n"
          "Examples:\n"
          "  smtp save host smtp.gmail.com port 465 sender me@gmail.com key <app_pw>\n"
          "  smtp save sender alerts@example.com key <new_key>",
      .handler = cli_save },
    { .name = "smtp.test",
      .summary = "Send a test mail to the configured sender",
      .usage   = "smtp test",
      .help_block =
          "Send a test mail FROM the configured sender TO itself.\n"
          "\n"
          "Use this after `smtp host` / `smtp port` / `smtp sender` /\n"
          "`smtp key` (or `smtp save`) to verify the server accepts the\n"
          "credentials and routes mail. The test runs asynchronously;\n"
          "watch the `smtp.send.end` event for the outcome.\n"
          "\n"
          "Examples:\n"
          "  smtp test",
      .handler = cli_test },
};

// ---------------------------------------------------------------------
// Factory reset hook
// ---------------------------------------------------------------------

// On device.factory-reset.requested: wipe ls_smtp NVS namespace + clear
// in-memory cfg (including the api_key, which is effectively the SMTP
// AUTH password). Pairing-completeness fix: previously the AUTH
// password / API key persisted, so a new owner inherited the previous
// owner's mail server credentials. Highest-priority leak of the five.
static void on_factory_reset(const sk_event_t *evt, void *user_ctx)
{
    (void)evt; (void)user_ctx;
    ESP_LOGW(TAG, "factory reset received — wiping ls_smtp NVS + api_key");

    // 1) Clear in-memory cfg (includes api_key zeroization).
    cfg_clear();

    // 2) Wipe NVS namespace.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------

esp_err_t ls_smtp_init(void)
{
    nvs_load();
    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); ++i) {
        sk_cli_register(&s_cmds[i]);
    }

    // Factory reset hook — wipe NVS + api_key.
    int sub;
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, &sub);

    ESP_LOGI(TAG, "init: host=\"%s\" port=%u sender=\"%s\" key=%s",
             s_cfg.host, (unsigned)s_cfg.port, s_cfg.sender,
             s_cfg.api_key[0] ? "set" : "unset");
    return ESP_OK;
}
