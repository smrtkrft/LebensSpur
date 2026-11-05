/*
 * http_auth — tarayıcı parolası (SHA-256(MAC:parola), ALTE ile aynı), oturum çerezi, IP başına kilit.
 * Parola yoksa koruma kapalıdır (ALTE); GUI bunu kalıcı uyarıyla gösterir.
 */
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "cfg.h"
#include "evlog.h"
#include "http_priv.h"

#define SESSIONS   4
#define LOCK_SLOTS 8
#define MAX_FAILS  5
#define COOKIE     "ls_sess"

/* last_us: son kullanım. Geçerlilik her denetimde GÜNCEL ttl ile hesaplanır → ttl kısaltılınca açık oturumlar da
 * hemen yeni süreye tabi olur (2026-11-06 güvenlik testi: eski 'bitiş zamanı' saklamak bunu kaçırıyordu). */
typedef struct { char token[33]; int64_t last_us; } sess_t;
typedef struct { char ip[16]; uint8_t fails; int64_t until_us; int64_t last_us; } lock_t;

static sess_t s_sess[SESSIONS];
static lock_t s_lock[LOCK_SLOTS];

void http_auth_init(void) { memset(s_sess, 0, sizeof s_sess); }

static cfg_auth_t auth_cfg(void)
{
    cfg_auth_t a;
    cfg_copy_section(CFG_SEC_AUTH, &a);
    return a;
}

bool http_auth_enabled(void) { return auth_cfg().hash[0] != 0; }

void http_auth_hash(const char *password, char out[65])
{
    uint8_t mac[6], dig[32];
    esp_efuse_mac_get_default(mac);
    char in[160];
    int n = snprintf(in, sizeof in, "%02X%02X%02X%02X%02X%02X:%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     password);
    mbedtls_sha256((const unsigned char *)in, (size_t)n, dig, 0);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", dig[i]);
    memset(in, 0, sizeof in);
}

static bool ct_equal(const char *a, const char *b)       /* sabit zamanlı karşılaştırma */
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = (unsigned char)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

static bool cookie_token(http_req_t *r, char out[33])
{
    size_t len = sizeof(char) * 33;
    return httpd_req_get_cookie_val(r->req, COOKIE, out, &len) == ESP_OK && strlen(out) == 32;
}

bool http_auth_session_valid(http_req_t *r)
{
    char t[33];
    if (!cookie_token(r, t)) return false;
    int64_t now = esp_timer_get_time();
    int64_t ttl = (int64_t)auth_cfg().ttl_min * 60 * 1000000LL;
    for (int i = 0; i < SESSIONS; i++) {
        if (s_sess[i].token[0] && now - s_sess[i].last_us < ttl && ct_equal(s_sess[i].token, t)) {
            s_sess[i].last_us = now;                           /* kayan süre */
            return true;
        }
    }
    return false;
}

/* SSE: akışı açan oturum hâlâ geçerli mi (süreyi uzatmaz: açık sekme oturumu canlı tutmasın) */
bool http_auth_token_alive(const char *tok)
{
    if (!http_auth_enabled()) return true;
    if (!tok || !tok[0]) return false;
    int64_t now = esp_timer_get_time();
    int64_t ttl = (int64_t)auth_cfg().ttl_min * 60 * 1000000LL;
    for (int i = 0; i < SESSIONS; i++) {
        if (s_sess[i].token[0] && now - s_sess[i].last_us < ttl && ct_equal(s_sess[i].token, tok)) return true;
    }
    return false;
}

void http_auth_cookie(http_req_t *r, char out[33])
{
    if (!cookie_token(r, out)) out[0] = 0;
}

static lock_t *lock_slot(const char *ip, bool create)
{
    lock_t *old = &s_lock[0];
    for (int i = 0; i < LOCK_SLOTS; i++) {
        if (strcmp(s_lock[i].ip, ip) == 0) return &s_lock[i];
        if (s_lock[i].last_us < old->last_us) old = &s_lock[i];
    }
    if (!create) return NULL;
    memset(old, 0, sizeof *old);
    strlcpy(old->ip, ip, sizeof old->ip);
    return old;
}

uint32_t http_auth_locked_s(http_req_t *r)
{
    lock_t *l = lock_slot(r->ip, false);
    int64_t now = esp_timer_get_time();
    return (l && l->until_us > now) ? (uint32_t)((l->until_us - now) / 1000000 + 1) : 0;
}

esp_err_t http_auth_login(http_req_t *r, const char *password)
{
    cfg_auth_t a = auth_cfg();
    if (!a.hash[0]) return ESP_OK;
    if (http_auth_locked_s(r)) return ESP_ERR_INVALID_STATE;
    char h[65];
    http_auth_hash(password, h);
    lock_t *l = lock_slot(r->ip, true);
    int64_t now = esp_timer_get_time();
    l->last_us = now;
    if (!ct_equal(h, a.hash)) {
        if (++l->fails >= MAX_FAILS && a.protect) {
            l->until_us = now + (int64_t)a.lock_min * 60 * 1000000LL;
            l->fails = 0;
            evlog_write(EVL_WARN, "user", "giris kilitlendi (%s, %u dk)", r->ip, a.lock_min);
        }
        return ESP_ERR_INVALID_ARG;
    }
    l->fails = 0;
    sess_t *s = &s_sess[0];
    for (int i = 0; i < SESSIONS; i++) if (s_sess[i].last_us < s->last_us) s = &s_sess[i];   /* en eski yuva */
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof rnd);
    for (int i = 0; i < 16; i++) sprintf(s->token + i * 2, "%02x", rnd[i]);
    s->last_us = now;
    char ck[96];
    snprintf(ck, sizeof ck, COOKIE "=%.32s; Path=/; HttpOnly; SameSite=Strict", s->token);
    http_set_header(r, "Set-Cookie", ck);                     /* değer istek bağlamına kopyalanır (W1) */
    evlog_write(EVL_INFO, "user", "giris (%s)", r->ip);
    return ESP_OK;
}

void http_auth_logout(http_req_t *r)
{
    char t[33];
    if (cookie_token(r, t)) {
        for (int i = 0; i < SESSIONS; i++) if (ct_equal(s_sess[i].token, t)) memset(&s_sess[i], 0, sizeof s_sess[i]);
    }
    http_set_header(r, "Set-Cookie", COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    http_sse_broadcast();                                     /* geçersiz oturumun akışı kapanır */
}

esp_err_t http_auth_set_password(const char *current, const char *next)
{
    cfg_auth_t a = auth_cfg();
    if (a.hash[0]) {
        char h[65];
        http_auth_hash(current ? current : "", h);
        if (!ct_equal(h, a.hash)) return ESP_ERR_INVALID_ARG;
    }
    if (next[0] && strlen(next) < 4) return ESP_ERR_INVALID_SIZE;
    char nh[65] = "";
    if (next[0]) http_auth_hash(next, nh);
    /* hash internal alan: API'den değil, yalnız buradan yazılır */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "hash", nh);
    esp_err_t r = cfg_set_section_internal(CFG_SEC_AUTH, o);
    cJSON_Delete(o);
    if (r == ESP_OK) {
        memset(s_sess, 0, sizeof s_sess);                     /* parola değişti: tüm oturumlar düşer */
        http_sse_broadcast();                                 /* açık SSE akışları da kapanır */
        evlog_write(EVL_INFO, "user", "tarayici parolasi %s", nh[0] ? "ayarlandi" : "kaldirildi");
    }
    return r;
}
