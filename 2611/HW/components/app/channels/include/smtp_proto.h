/*
 * smtp_proto — SMTP protokol yardımcıları (saf C; masaüstünde test edilir). 03-arastirma/02 §3.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "outbox.h"          /* channel_result_t */

typedef struct {
    bool auth_plain, auth_login, starttls, eightbit;
    uint32_t size_max;       /* 0 = bildirilmedi */
} smtp_caps_t;

/* "250-..." / "250 ..." → kod ve son satır mı. Bozuksa -1. */
int  smtp_parse_line(const char *line, int *code, bool *last);
/* EHLO yanıt satırının metin kısmını (kod sonrası) yetenek olarak işler */
void smtp_ehlo_cap(const char *text, smtp_caps_t *caps);
/* yanıt kodu → teslimat sonucu: 2xx/3xx OK; 4xx RETRY; 530/534/535/538 CONFIG; diğer 5xx PERM */
channel_result_t smtp_classify(int code);
/* gövde: tek \n → \r\n, nokta ile başlayan satır çiftlenir. Sığmazsa 0. */
size_t smtp_body_escape(const char *in, char *out, size_t cap);
/* konu: ASCII ise aynen, değilse RFC 2047 =?UTF-8?B?…?= (UTF-8 karakter bölünmez). Sığmazsa 0. */
size_t smtp_encode_subject(const char *in, char *out, size_t cap);
/* base64; out NUL sonlu. Dönüş uzunluk ya da sığmazsa 0. */
size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
/* base64, 76 karakterlik CRLF satırlarıyla (MIME ek parçası; n 57'nin katı olmalı, son parça hariç) */
size_t b64_encode_lines(const uint8_t *in, size_t n, char *out, size_t cap);
/* AUTH PLAIN jetonu: base64("\0user\0pass") */
size_t smtp_auth_plain(const char *user, const char *pass, char *out, size_t cap);
