/*
 * ch_smtp — SMTP gönderimi (03-arastirma/02 §1, §3). net_tls üzerinden; 465 TLS, diğerleri STARTTLS.
 * Sırlar hiçbir log seviyesinde basılmaz. Tüm adımlar tek son tarihe bağlı.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_timer.h"
#include "cfg.h"
#include "channel.h"
#include "evlog.h"
#include "net_tls.h"
#include "net_wifi.h"
#include "smtp_proto.h"
#include "storage.h"

#define BASE_US        (120LL * 1000000LL)
#define PER_ATTACH_US  (60LL * 1000000LL)
#define MAX_ATTACH     5
#define CHUNK_RAW      2964                     /* 52 × 57: tek TLS kaydına sığan base64 parçası */
#define WORK           4800
#define BOUNDARY       "----=_LebensSpur_2611"

typedef struct {
    net_conn_t *c;
    int64_t deadline;
    char *buf;                                   /* WORK bayt: satır/gövde/parça tamponu */
    char *err;
    size_t errlen;
} smtp_t;

static channel_result_t fail(smtp_t *s, channel_result_t r, const char *what, int code)
{
    if (code) snprintf(s->err, s->errlen, "%s: %d", what, code);
    else snprintf(s->err, s->errlen, "%s", what);
    return r;
}

/* çok satırlı yanıtı okur; EHLO ise yetenekleri işler. Dönüş: son kod ya da <0 */
static int reply(smtp_t *s, smtp_caps_t *caps)
{
    char line[256];
    for (;;) {
        if (net_readline(s->c, line, sizeof line, s->deadline) < 0) return -1;
        int code;
        bool last;
        if (smtp_parse_line(line, &code, &last) < 0) return -2;
        if (caps && line[3]) smtp_ehlo_cap(line + 4, caps);
        if (last) return code;
    }
}

static int cmd(smtp_t *s, const char *text, smtp_caps_t *caps)
{
    if (net_write_all(s->c, text, strlen(text), s->deadline) < 0) return -1;
    return reply(s, caps);
}

static bool send_str(smtp_t *s, const char *t) { return net_write_all(s->c, t, strlen(t), s->deadline) >= 0; }

/* alıcı listesi: virgül, noktalı virgül ya da boşlukla ayrılmış */
static int next_rcpt(const char **p, char *out, size_t cap)
{
    while (**p == ',' || **p == ';' || **p == ' ') (*p)++;
    size_t n = 0;
    while (**p && **p != ',' && **p != ';' && **p != ' ' && n + 1 < cap) out[n++] = *(*p)++;
    out[n] = 0;
    return (int)n;
}

static bool b64_text(smtp_t *s, const char *text)
{
    /* metni CRLF'e normalize et, base64 satırlarına çevir (nokta sorunu yok) */
    size_t len = strlen(text);
    char *norm = malloc(len * 2 + 4);
    if (!norm) return false;
    size_t o = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') norm[o++] = '\r';
        norm[o++] = *p;
    }
    bool ok = true;
    for (size_t i = 0; i < o && ok; i += CHUNK_RAW) {
        size_t n = o - i < CHUNK_RAW ? o - i : CHUNK_RAW;
        ok = b64_encode_lines((const uint8_t *)norm + i, n, s->buf, WORK) && send_str(s, s->buf);
    }
    free(norm);
    return ok;
}

/* Ek dosyası parça parça okunur; çip kilidi yalnız her parçanın okunması sırasında tutulur
 * (oturum dakikalar sürebilir, günlük yazımı aradaki boşluklarda ilerlesin). */
static bool send_attachment(smtp_t *s, const char *path, const char *name, char *missing, size_t mlen)
{
    snprintf(s->buf, WORK,
             "\r\n--" BOUNDARY "\r\nContent-Type: application/octet-stream; name=\"%s\"\r\n"
             "Content-Transfer-Encoding: base64\r\nContent-Disposition: attachment; filename=\"%s\"\r\n\r\n",
             name, name);
    if (!send_str(s, s->buf)) return false;
    static uint8_t raw[CHUNK_RAW];
    long off = 0;
    for (;;) {
        size_t n = 0;
        bool opened = false;
        if (storage_acquire(STG_DATA)) {
            FILE *f = fopen(path, "rb");
            if (f) {
                opened = true;
                if (fseek(f, off, SEEK_SET) == 0) n = fread(raw, 1, sizeof raw, f);
                fclose(f);
            }
            storage_release(STG_DATA);
        }
        if (!opened) {                               /* gönderim sırasında silindi/okunamadı: not düş */
            size_t m = strlen(missing);
            snprintf(missing + m, mlen - m, "%s%.60s", m ? ", " : "", name);
            return true;
        }
        if (n == 0) return true;
        off += (long)n;
        if (!b64_encode_lines(raw, n, s->buf, WORK) || !send_str(s, s->buf)) return false;
    }
}

channel_result_t ch_smtp_send(const channel_ctx_t *ctx, char *err, size_t errlen)
{
    static cfg_smtp_t sm;
    static cfg_early_mail_t em;
    static cfg_group_t grp;
    cfg_copy_section(CFG_SEC_SMTP, &sm);
    if (!sm.server[0] || !sm.user[0]) return (snprintf(err, errlen, "smtp ayari yok"), CR_CONFIG);
    const ob_job_t *j = ctx->job;
    const char *rcpts, *subj_t, *body_t;
    bool group = j->kind == JK_TRIGGER;
    if (group) {
        if (cfg_group_get(j->group, &grp) != ESP_OK) return (snprintf(err, errlen, "grup yok"), CR_PERM);
        rcpts = grp.recipients; subj_t = grp.subject; body_t = grp.body;
    } else {
        cfg_copy_section(CFG_SEC_EARLY_MAIL, &em);
        rcpts = em.email[0] ? em.email : sm.user;
        subj_t = j->kind == JK_TEST ? "LebensSpur test" : em.subject;
        body_t = j->kind == JK_TEST ? "LebensSpur: SMTP test OK." : em.body;
    }
    if (!subj_t[0]) subj_t = "LebensSpur: {status} {k}/{n}";
    if (!body_t[0]) body_t = "LebensSpur {device}: {status}, {remaining}.";

    smtp_t s = { .err = err, .errlen = errlen };
    s.buf = malloc(WORK);
    if (!s.buf) return (snprintf(err, errlen, "bellek"), CR_RETRY);

    /* ekler (yalnız tetikleme) */
    char paths[MAX_ATTACH][96], names[MAX_ATTACH][64];
    int na = 0;
    if (group && storage_acquire(STG_DATA)) {        /* yalnız listeleme sırasında çip kilidi */
        char dir[24];
        snprintf(dir, sizeof dir, STG_DATA "/mail/g%u", (unsigned)(j->group % 100));
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)) && na < MAX_ATTACH) {
            if (e->d_name[0] == '.') continue;
            snprintf(paths[na], sizeof paths[0], "%s/%.60s", dir, e->d_name);
            snprintf(names[na], sizeof names[0], "%.63s", e->d_name);
            na++;
        }
        if (d) closedir(d);
        storage_release(STG_DATA);
    }
    s.deadline = esp_timer_get_time() + BASE_US + na * PER_ATTACH_US;

    tmpl_vars_t v;
    char tb[32], rb[32];
    channel_vars(ctx, &v, tb, sizeof tb, rb, sizeof rb);
    static char subj[160], subj_enc[400], body[2200];
    tmpl_expand(subj_t, &v, subj, sizeof subj, false);
    tmpl_expand(body_t, &v, body, sizeof body, false);
    smtp_encode_subject(subj, subj_enc, sizeof subj_enc);

    net_tls_opts_t o = NET_TLS_OPTS_DEFAULT;
    o.verify = sm.tls_verify;
    bool direct = sm.port == 465;
    channel_result_t res = CR_RETRY;
    smtp_caps_t caps = { 0 };
    int code;
    char missing[160] = "";
    wifi_status_t w;
    net_wifi_get_status(&w);

    s.c = direct ? net_connect_tls(sm.server, sm.port, &o, err, errlen)
                 : net_connect_plain(sm.server, sm.port, &o, err, errlen);
    if (!s.c) goto out;                                   /* err dolduruldu: RETRY */
    if ((code = reply(&s, NULL)) != 220) { res = fail(&s, code < 0 ? CR_RETRY : smtp_classify(code), "karsilama", code); goto out; }
    snprintf(s.buf, WORK, "EHLO [%s]\r\n", w.ip[0] ? w.ip : "127.0.0.1");
    if ((code = cmd(&s, s.buf, &caps)) != 250) { res = fail(&s, code < 0 ? CR_RETRY : smtp_classify(code), "EHLO", code); goto out; }
    if (!direct) {
        if (!caps.starttls) { res = fail(&s, CR_CONFIG, "sunucu STARTTLS sunmuyor (parola duz metin gonderilmez)", 0); goto out; }
        if ((code = cmd(&s, "STARTTLS\r\n", NULL)) != 220) { res = fail(&s, CR_RETRY, "STARTTLS", code); goto out; }
        if (net_starttls(s.c, sm.server, &o, err, errlen) != ESP_OK) { res = CR_RETRY; goto out; }
        memset(&caps, 0, sizeof caps);
        if ((code = cmd(&s, s.buf, &caps)) != 250) { res = fail(&s, CR_RETRY, "EHLO2", code); goto out; }
    }
    if (caps.auth_plain || !caps.auth_login) {
        char tok[200];
        smtp_auth_plain(sm.user, sm.pass, tok, sizeof tok);
        snprintf(s.buf, WORK, "AUTH PLAIN %s\r\n", tok);
        memset(tok, 0, sizeof tok);
        code = cmd(&s, s.buf, NULL);
    } else {
        char tok[160];
        code = cmd(&s, "AUTH LOGIN\r\n", NULL);
        if (code == 334) {
            b64_encode((const uint8_t *)sm.user, strlen(sm.user), tok, sizeof tok);
            snprintf(s.buf, WORK, "%s\r\n", tok);
            code = cmd(&s, s.buf, NULL);
        }
        if (code == 334) {
            b64_encode((const uint8_t *)sm.pass, strlen(sm.pass), tok, sizeof tok);
            snprintf(s.buf, WORK, "%s\r\n", tok);
            code = cmd(&s, s.buf, NULL);
        }
        memset(tok, 0, sizeof tok);
    }
    memset(s.buf, 0, WORK);                               /* parola içeren tampon temizlenir */
    if (code != 235) { res = fail(&s, code < 0 ? CR_RETRY : smtp_classify(code), "kimlik dogrulama", code); goto out; }
    snprintf(s.buf, WORK, "MAIL FROM:<%s>\r\n", sm.user);
    if ((code = cmd(&s, s.buf, NULL)) != 250) { res = fail(&s, code < 0 ? CR_RETRY : smtp_classify(code), "MAIL FROM", code); goto out; }

    int accepted = 0, rejected = 0;
    char addr[96], to_hdr[520] = "";
    for (const char *p = rcpts; next_rcpt(&p, addr, sizeof addr) > 0;) {
        snprintf(s.buf, WORK, "RCPT TO:<%s>\r\n", addr);
        code = cmd(&s, s.buf, NULL);
        if (code < 0) { res = fail(&s, CR_RETRY, "RCPT", code); goto out; }
        if (code == 250 || code == 251) {
            accepted++;
            size_t l = strlen(to_hdr);
            snprintf(to_hdr + l, sizeof to_hdr - l, "%s%s", l ? ", " : "", addr);
        } else {
            rejected++;
            evlog_write(EVL_WARN, "mail", "alici reddedildi (%d): %s", code, addr);
        }
    }
    if (!accepted) { res = fail(&s, CR_PERM, rejected ? "hicbir alici kabul edilmedi" : "alici yok", 0); goto out; }
    if ((code = cmd(&s, "DATA\r\n", NULL)) != 354) { res = fail(&s, code < 0 ? CR_RETRY : smtp_classify(code), "DATA", code); goto out; }

    char date[48] = "";
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm g;
        gmtime_r(&now, &g);
        strftime(date, sizeof date, "Date: %a, %d %b %Y %H:%M:%S +0000\r\n", &g);
    }
    int hl = snprintf(s.buf, WORK,
                      "From: <%s>\r\nTo: %s\r\nSubject: %s\r\n%sMessage-ID: <ls-%u-%u-g%u@%s.local>\r\nMIME-Version: 1.0\r\n",
                      sm.user, to_hdr, subj_enc, date, ctx->trigger_seq, j->kind, j->group, net_wifi_device_name());
    if (na) snprintf(s.buf + hl, WORK - (size_t)hl, "Content-Type: multipart/mixed; boundary=\"" BOUNDARY "\"\r\n\r\n--" BOUNDARY
                     "\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n");
    else snprintf(s.buf + hl, WORK - (size_t)hl, "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n");
    if (!send_str(&s, s.buf)) { res = fail(&s, CR_RETRY, "baslik", 0); goto out; }

    /* ekleri önce yokla: eksik olanlar gövdeye not olarak eklenir (karar 9) */
    bool fs = na && storage_acquire(STG_DATA);
    for (int i = 0; i < na; i++) {
        struct stat st;
        if (!fs || stat(paths[i], &st) != 0) {
            size_t m = strlen(missing);
            snprintf(missing + m, sizeof missing - m, "%s%.60s", m ? ", " : "", names[i]);
            paths[i][0] = 0;
        }
    }
    if (fs) storage_release(STG_DATA);
    if (missing[0]) {
        size_t bl = strlen(body);
        snprintf(body + bl, sizeof body - bl, "\n\n[LebensSpur] Ek okunamadi / attachment unavailable: %s", missing);
    }
    if (!b64_text(&s, body)) { res = fail(&s, CR_RETRY, "govde", 0); goto out; }
    for (int i = 0; i < na; i++) {
        if (paths[i][0] && !send_attachment(&s, paths[i], names[i], missing, sizeof missing)) {
            res = fail(&s, CR_RETRY, "ek gonderimi", 0);
            goto out;
        }
    }
    if (na && !send_str(&s, "\r\n--" BOUNDARY "--\r\n")) { res = fail(&s, CR_RETRY, "sinir", 0); goto out; }
    if ((code = cmd(&s, "\r\n.\r\n", NULL)) != 250) { res = fail(&s, code < 0 ? CR_RETRY : smtp_classify(code), "gonderim", code); goto out; }
    res = CR_OK;
    if (missing[0]) evlog_write(EVL_WARN, "mail", "eksik ek bildirildi: %s", missing);
    cmd(&s, "QUIT\r\n", NULL);

out:
    net_close(s.c);
    free(s.buf);
    return res;
}
