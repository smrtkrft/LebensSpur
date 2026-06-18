#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// ls_smtp - minimal SMTPS (SSL on port 465) client
//
// Phase 1.3 initial implementation. Scope:
//   - SMTPS only (port 465, TLS at connect). STARTTLS (port 587) not
//     supported.
//   - AUTH LOGIN (base64 user + base64 password). PLAIN / CRAM-MD5 not
//     supported.
//   - Single message send: subject + plain text body + recipient list.
//   - No attachments (out of phase 1 scope).
//
// Config is stored in NVS (host/port/sender/api_key). Each ls_smtp_send
// call opens a fresh TLS connection, transmits, and tears it down. It is
// invoked from a worker task asynchronously (mail_groups already does
// this); a direct synchronous call is also possible but blocking, so
// avoid calling it from event handlers.
//
// CLI commands (per-property setters, no --flags):
//   smtp.host    - SMTPS server hostname
//   smtp.port    - SMTPS port (1..65535)
//   smtp.sender  - sender email address
//   smtp.key     - SMTP AUTH password / App Password / API key
//   smtp.get     - show configuration (api_key masked)
//   smtp.test    - send a test mail from the sender to itself
//
// Event publications:
//   smtp.send.start  {"to":"...","subject":"..."}
//   smtp.send.end    {"ok":true|false,"err":"..."}
// =====================================================================

#define LS_SMTP_HOST_MAX    127
#define LS_SMTP_SENDER_MAX  127
#define LS_SMTP_KEY_MAX     191
#define LS_SMTP_MAX_RCPT    32

typedef struct {
    char     host[LS_SMTP_HOST_MAX + 1];
    uint16_t port;                       // default 465
    char     sender[LS_SMTP_SENDER_MAX + 1];
    char     api_key[LS_SMTP_KEY_MAX + 1];
} ls_smtp_config_t;

esp_err_t ls_smtp_init(void);

// Synchronous send. `recipients` is an array of NUL-terminated strings.
// `subject` and `body` are NUL-terminated. Body is plain text (CRLF
// allowed).
// Returns SK_OK or one of SK_ERR_SMTP_*.
sk_err_t  ls_smtp_send(const char *subject,
                       const char *body,
                       const char *const *recipients,
                       int n_recipients);

void ls_smtp_config(ls_smtp_config_t *out);  // api_key returned in full (handler use)
bool ls_smtp_is_configured(void);

#ifdef __cplusplus
}
#endif
