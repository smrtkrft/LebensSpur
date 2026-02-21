#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* SMTP yapilandirma */
typedef struct {
    char server[64];
    uint16_t port;
    char user[64];
    char api_key[64];    /* Gmail App Password vb. */
} smtp_config_t;

/* Baslatma: NVS'den kayitli ayarlari yukler */
esp_err_t smtp_client_init(void);

/* Ayarlari kaydet (NVS) */
esp_err_t smtp_client_save_config(const smtp_config_t *cfg);

/* Kayitli ayarlari oku */
esp_err_t smtp_client_get_config(smtp_config_t *cfg);

/* Ayar var mi? */
bool smtp_client_has_config(void);

/* Test maili gonder (bloklayan cagri, ayri task'ta calistirin) */
esp_err_t smtp_client_send_test(void);

/* Genel mail gonder */
esp_err_t smtp_client_send(const char *to, const char *subject, const char *body);

/* Dosya ekli mail gonder (MIME multipart)
 * file_paths: "/ext/mail/g0/doc.pdf" gibi mutlak dosya yollari dizisi
 * file_count: dosya sayisi (0 ise ek olmadan gonderir) */
esp_err_t smtp_client_send_with_attachments(const char *to, const char *subject,
                                             const char *body,
                                             const char **file_paths, int file_count);
