#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// ls_mail_groups - LebensSpur trigger-mail group manager
//
// Up to 10 groups. Each group holds name + subject + body + recipient
// list. On timer.triggered every enabled group is sent automatically
// (sequentially via a worker task; ls_smtp_send is synchronous).
//
// Phase 1.3 scope: no attachments, plain subject + body only.
//
// CLI commands (per-property setters, no --flags):
//   mail.group.add               - create a new group, returns id
//   mail.group.delete            - delete a group
//   mail.group.enable            - enable/disable a group (on|off)
//   mail.group.name              - rename a group
//   mail.group.subject           - set the email subject (multi-word)
//   mail.group.body              - set the email body (multi-word)
//   mail.group.list              - summary list
//   mail.group.get               - full detail of one group
//   mail.group.recipient.add     - add a recipient email
//   mail.group.recipient.remove  - remove a recipient
//
// NVS namespace "ls_mg". Each group uses key prefix "g<i>_" + field
// name.
// =====================================================================

#define LS_MAIL_GROUP_MAX             10
#define LS_MAIL_GROUP_NAME_MAX        47
#define LS_MAIL_GROUP_SUBJECT_MAX     127
#define LS_MAIL_GROUP_BODY_MAX        511
#define LS_MAIL_GROUP_RCPT_MAX        20   // max recipients per group
#define LS_MAIL_GROUP_RCPT_EMAIL_MAX  95

typedef struct {
    bool     used;
    bool     enabled;     // disabled groups are skipped on timer.triggered
    char     name[LS_MAIL_GROUP_NAME_MAX + 1];
    char     subject[LS_MAIL_GROUP_SUBJECT_MAX + 1];
    char     body[LS_MAIL_GROUP_BODY_MAX + 1];
    char     recipients[LS_MAIL_GROUP_RCPT_MAX][LS_MAIL_GROUP_RCPT_EMAIL_MAX + 1];
    uint8_t  recipient_count;
} ls_mail_group_t;

esp_err_t ls_mail_groups_init(void);

// Snapshot - id 0..LS_MAIL_GROUP_MAX-1.
void ls_mail_groups_get(int id, ls_mail_group_t *out);

// Find an empty slot and create a new group; returns id or -1 (full).
int ls_mail_groups_add(const char *name);

// Delete.
esp_err_t ls_mail_groups_delete(int id);

#ifdef __cplusplus
}
#endif
