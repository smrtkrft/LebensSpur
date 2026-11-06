#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smtp_proto.h"

int smtp_parse_line(const char *l, int *code, bool *last)
{
    if (!isdigit((unsigned char)l[0]) || !isdigit((unsigned char)l[1]) || !isdigit((unsigned char)l[2])) return -1;
    if (l[3] != ' ' && l[3] != '-' && l[3] != 0) return -1;
    *code = (l[0] - '0') * 100 + (l[1] - '0') * 10 + (l[2] - '0');
    *last = l[3] != '-';
    return 0;
}

void smtp_ehlo_cap(const char *t, smtp_caps_t *c)
{
    if (strncasecmp(t, "AUTH", 4) == 0 && (t[4] == ' ' || t[4] == '=')) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", t + 5);
        for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
            if (strcasecmp(tok, "PLAIN") == 0) c->auth_plain = true;
            if (strcasecmp(tok, "LOGIN") == 0) c->auth_login = true;
        }
    } else if (strcasecmp(t, "STARTTLS") == 0) {
        c->starttls = true;
    } else if (strcasecmp(t, "8BITMIME") == 0) {
        c->eightbit = true;
    } else if (strncasecmp(t, "SIZE", 4) == 0) {
        c->size_max = (uint32_t)strtoul(t + 4, NULL, 10);
    }
}

channel_result_t smtp_classify(int code)
{
    if (code >= 200 && code < 400) return CR_OK;
    if (code >= 400 && code < 500) return CR_RETRY;
    if (code == 530 || code == 534 || code == 535 || code == 538) return CR_CONFIG;
    return CR_PERM;
}

size_t smtp_body_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    bool bol = true;                                   /* satır başı */
    for (const char *p = in; *p; p++) {
        if (*p == '\r') continue;                      /* CRLF'i tek biçime indir */
        if (bol && *p == '.') { if (o + 1 >= cap) return 0; out[o++] = '.'; }
        if (*p == '\n') {
            if (o + 2 >= cap) return 0;
            out[o++] = '\r';
            out[o++] = '\n';
            bol = true;
            continue;
        }
        if (o + 1 >= cap) return 0;
        out[o++] = *p;
        bol = false;
    }
    out[o] = 0;
    return o;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t need = (n + 2) / 3 * 4;
    if (need + 1 > cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

size_t b64_encode_lines(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 57) {
        size_t chunk = n - i < 57 ? n - i : 57;
        size_t w = b64_encode(in + i, chunk, out + o, cap - o);
        if (!w || o + w + 3 > cap) return 0;
        o += w;
        out[o++] = '\r';
        out[o++] = '\n';
    }
    out[o] = 0;
    return o;
}

size_t smtp_encode_subject(const char *in, char *out, size_t cap)
{
    bool ascii = true;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (*p >= 0x80 || *p < 0x20) ascii = false;
    }
    if (ascii) {
        size_t n = strlen(in);
        if (n + 1 > cap) return 0;
        memcpy(out, in, n + 1);
        return n;
    }
    /* her kodlanmış sözcük ≤ 75 kr: 45 bayt ham → 60 base64 + 12 sarmal */
    size_t o = 0, len = strlen(in), i = 0;
    while (i < len) {
        size_t take = len - i < 45 ? len - i : 45;
        while (take < len - i && take > 0 && ((unsigned char)in[i + take] & 0xC0) == 0x80) take--;   /* UTF-8 sınırı */
        if (o && o + 3 < cap) { out[o++] = '\r'; out[o++] = '\n'; out[o++] = ' '; }
        int n = snprintf(out + o, cap - o, "=?UTF-8?B?");
        if (n < 0 || o + (size_t)n >= cap) return 0;
        o += (size_t)n;
        size_t w = b64_encode((const uint8_t *)in + i, take, out + o, cap - o);
        if (!w || o + w + 3 > cap) return 0;
        o += w;
        out[o++] = '?';
        out[o++] = '=';
        out[o] = 0;
        i += take;
    }
    return o;
}

size_t smtp_auth_plain(const char *user, const char *pass, char *out, size_t cap)
{
    uint8_t buf[160];
    size_t ul = strlen(user), pl = strlen(pass);
    if (ul + pl + 2 > sizeof buf) return 0;
    buf[0] = 0;
    memcpy(buf + 1, user, ul);
    buf[1 + ul] = 0;
    memcpy(buf + 2 + ul, pass, pl);
    size_t r = b64_encode(buf, ul + pl + 2, out, cap);
    memset(buf, 0, sizeof buf);
    return r;
}
