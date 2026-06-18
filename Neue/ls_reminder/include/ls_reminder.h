#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// ls_reminder - LebensSpur early-warning reminder mail
//
// Single config: enabled flag + subject + body + recipient list. On
// timer.alarm (the early-warning thresholds, fired N times as the
// deadline approaches) the reminder mail is sent automatically to every
// recipient. Device-owned: config lives in NVS, firing needs no app
// involvement and works while the SKAPP app is closed.
//
// This is the early-warning counterpart to ls_mail_groups (which fires on
// timer.triggered, the final consequence). Reminders nudge the user
// ("you haven't reset yet"); mail groups are the actual trigger action.
//
// CLI commands (per-property setters, no --flags):
//   reminder.enable            - enable/disable reminders (on|off)
//   reminder.subject           - set the email subject (multi-word)
//   reminder.body              - set the email body (multi-word)
//   reminder.recipient.add     - add a recipient email (max 10)
//   reminder.recipient.remove  - remove a recipient
//   reminder.get               - show configuration
//   reminder.test              - send the reminder mail once, now
//
// Event publications:
//   reminder.fire  {"ok":true|false,"index":i,"of":N[,"err":"ERR_*"]}
//
// NVS namespace "ls_rem". Flat per-field keys (en / sb / bd / rc / r0..r9).
// =====================================================================

#define LS_REMINDER_SUBJECT_MAX     127
#define LS_REMINDER_BODY_MAX        511
#define LS_REMINDER_RCPT_MAX        10   // max recipients
#define LS_REMINDER_RCPT_EMAIL_MAX  95

esp_err_t ls_reminder_init(void);

#ifdef __cplusplus
}
#endif
