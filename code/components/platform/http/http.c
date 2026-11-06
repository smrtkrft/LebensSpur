/*
 * http — httpd kurulumu, joker yönlendirme, ön işleyici, yanıt zarfı, gömülü statik dosyalar.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_log.h"
#include "evlog.h"
#include "http_priv.h"

static const char *TAG = "http";

static httpd_handle_t s_server;
static const http_route_t *s_routes;
static size_t s_nroutes;
static http_req_t s_ctx;                  /* tek istek bir anda (httpd tek görev) */
static char s_body[HTTP_BODY_MAX + 1];
static char s_resp[HTTP_RESP_MAX + 8];

httpd_handle_t http_server(void) { return s_server; }

/* ---------- yanıt ---------- */

void http_set_header(http_req_t *r, const char *name, const char *value)
{
    if (r->nhdr >= HTTP_MAX_HDRS) return;
    strlcpy(r->hdr_name[r->nhdr], name, sizeof r->hdr_name[0]);
    strlcpy(r->hdr_val[r->nhdr], value, sizeof r->hdr_val[0]);
    httpd_resp_set_hdr(r->req, r->hdr_name[r->nhdr], r->hdr_val[r->nhdr]);
    r->nhdr++;
}

/* Güvenlik başlıkları (test_security.py): gömülme yok (clickjacking), MIME tahmini yok, Referer sızmaz.
 * CSP yalnız HTML'de: betik/bağlantı yalnız cihazın kendisi; satır içi stil ALTE düzeni için gerekli. */
static void sec_headers(httpd_req_t *req, bool html)
{
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    if (html) {
        httpd_resp_set_hdr(req, "Content-Security-Policy",
                           "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                           "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; "
                           "form-action 'self'; object-src 'none'");
    }
}

static esp_err_t send_json(http_req_t *r, const char *status, cJSON *o)
{
    sec_headers(r->req, false);
    httpd_resp_set_status(r->req, status);
    httpd_resp_set_type(r->req, "application/json");
    httpd_resp_set_hdr(r->req, "Cache-Control", "no-store");
    esp_err_t e;
    if (cJSON_PrintPreallocated(o, s_resp, HTTP_RESP_MAX, false)) {
        e = httpd_resp_send(r->req, s_resp, HTTPD_RESP_USE_STRLEN);
    } else {
        char *p = cJSON_PrintUnformatted(o);            /* büyük yanıt: heap */
        e = p ? httpd_resp_send(r->req, p, HTTPD_RESP_USE_STRLEN) : httpd_resp_send_500(r->req);
        cJSON_free(p);
    }
    cJSON_Delete(o);
    return e;
}

esp_err_t http_ok(http_req_t *r, cJSON *data)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateObject());
    return send_json(r, "200 OK", o);
}

esp_err_t http_fail(http_req_t *r, int status, const char *code, const char *msg, const char *field)
{
    static const struct { int s; const char *t; } st[] = {
        { 400, "400 Bad Request" }, { 401, "401 Unauthorized" }, { 403, "403 Forbidden" },
        { 404, "404 Not Found" }, { 405, "405 Method Not Allowed" }, { 409, "409 Conflict" },
        { 413, "413 Payload Too Large" }, { 429, "429 Too Many Requests" }, { 503, "503 Service Unavailable" },
    };
    const char *line = "500 Internal Server Error";
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++) if (st[i].s == status) line = st[i].t;
    cJSON *o = cJSON_CreateObject(), *e = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg ? msg : "");
    if (field) cJSON_AddStringToObject(e, "field", field);
    cJSON_AddItemToObject(o, "error", e);
    return send_json(r, line, o);
}

/* ---------- istek yardımcıları ---------- */

const cJSON *http_body(http_req_t *r) { return r->body; }
httpd_req_t *http_raw(http_req_t *r) { return r->req; }
const char *http_client_ip(http_req_t *r) { return r->ip; }
const char *http_param(http_req_t *r, int i) { return (i >= 0 && i < HTTP_MAX_PARAMS) ? r->params[i] : NULL; }

int http_param_int(http_req_t *r, int i, int def)
{
    const char *p = http_param(r, i);
    if (!p || !*p) return def;
    int v = 0;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p) || v > 100000) return def;
        v = v * 10 + (*p - '0');
    }
    return v;
}

const char *http_query(http_req_t *r, const char *key, char *buf, size_t cap)
{
    char q[160];
    if (httpd_req_get_url_query_str(r->req, q, sizeof q) != ESP_OK) return NULL;
    return httpd_query_key_value(q, key, buf, cap) == ESP_OK ? buf : NULL;
}

static void fill_ip(http_req_t *r)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    r->ip[0] = 0;
    if (getpeername(httpd_req_to_sockfd(r->req), (struct sockaddr *)&a, &l) == 0 && a.sin_family == AF_INET) {
        inet_ntoa_r(a.sin_addr, r->ip, sizeof r->ip);
    }
}

/* ---------- CSRF / DNS rebinding ---------- */

static bool host_allowed(const char *host)
{
    char h[64];
    strlcpy(h, host, sizeof h);
    char *colon = strchr(h, ':');
    if (colon) *colon = 0;
    bool ip = h[0] != 0;
    for (const char *c = h; *c; c++) if (!isdigit((unsigned char)*c) && *c != '.') ip = false;
    if (ip) return true;                                  /* IP ile erişim (AP 192.168.4.1, LAN) */
    size_t n = strlen(h);
    if (n > 6 && strcasecmp(h + n - 6, ".local") == 0) h[n - 6] = 0;
    return strncasecmp(h, "ls-", 3) == 0 && strlen(h) == 13;   /* cihaz adı (mDNS) */
}

/* Geçerse NULL; geçmezse ret nedeni (çağıran 403 gönderir ve İŞLEYİCİYİ ÇAĞIRMAZ). */
static const char *csrf_reject(http_req_t *r)
{
    char host[64] = "", origin[96] = "", x[8] = "";
    if (httpd_req_get_hdr_value_str(r->req, "Host", host, sizeof host) != ESP_OK || !host_allowed(host)) return "host";
    if (httpd_req_get_hdr_value_str(r->req, "X-LS-Request", x, sizeof x) != ESP_OK || strcmp(x, "1") != 0) {
        return "csrf";
    }
    if (httpd_req_get_hdr_value_str(r->req, "Origin", origin, sizeof origin) == ESP_OK &&
        (strncmp(origin, "http://", 7) != 0 || strcasecmp(origin + 7, host) != 0)) {
        return "origin";
    }
    return NULL;
}

typedef enum { BODY_OK, BODY_REJECTED, BODY_SOCKET_ERR } body_res_t;

/* BODY_REJECTED: hata yanıtı gönderildi; BODY_SOCKET_ERR: bağlantı kapatılmalı. İkisinde de işleyici çağrılmaz. */
static body_res_t read_body(http_req_t *r)
{
    size_t len = r->req->content_len;
    if (len > HTTP_BODY_MAX) { http_fail(r, 413, "too_large", "en cok 2048 bayt", NULL); return BODY_REJECTED; }
    char ct[48] = "";
    httpd_req_get_hdr_value_str(r->req, "Content-Type", ct, sizeof ct);
    if (strncasecmp(ct, "application/json", 16) != 0) {
        http_fail(r, 400, "bad_request", "json bekleniyor", NULL);
        return BODY_REJECTED;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int n = httpd_req_recv(r->req, s_body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) return BODY_SOCKET_ERR;
        got += (size_t)n;
    }
    s_body[got] = 0;
    r->body = cJSON_ParseWithLength(s_body, got);
    if (!r->body) { http_fail(r, 400, "bad_request", "gecersiz json", NULL); return BODY_REJECTED; }
    return BODY_OK;
}

/* ---------- yönlendirme ---------- */

static bool match(const char *pat, const char *uri, http_req_t *r)
{
    int np = 0;
    while (*pat && *uri) {
        if (pat[0] == '{' && pat[1] == '}') {
            const char *end = uri;
            while (*end && *end != '/' && *end != '?') end++;
            size_t n = (size_t)(end - uri);
            if (n == 0 || n >= sizeof r->param_buf[0] || np >= HTTP_MAX_PARAMS) return false;
            memcpy(r->param_buf[np], uri, n);
            r->param_buf[np][n] = 0;
            r->params[np] = r->param_buf[np];
            np++;
            pat += 2;
            uri = end;
            continue;
        }
        if (*pat++ != *uri++) return false;
    }
    return *pat == 0 && (*uri == 0 || *uri == '?');
}

static esp_err_t serve_static(httpd_req_t *req)
{
    size_t n;
    const char *hash;
    const http_embedded_t *f = http_embedded_files(&n, &hash);
    char path[96];
    const char *uri = req->uri;
    bool hashed = false;
    if (strcmp(uri, "/") == 0 || strncmp(uri, "/?", 2) == 0) uri = "/index.html";
    if (strncmp(uri, "/s/", 3) == 0) {                     /* /s/<hash>/app.js → /s/app.js */
        const char *rest = strchr(uri + 3, '/');
        if (!rest) return httpd_resp_send_404(req);
        hashed = (size_t)(rest - (uri + 3)) == strlen(hash) && strncmp(uri + 3, hash, strlen(hash)) == 0;
        snprintf(path, sizeof path, "/s%s", rest);
        uri = path;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(uri, f[i].path) != 0) continue;
        httpd_resp_set_type(req, f[i].ctype);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
        httpd_resp_set_hdr(req, "Cache-Control", hashed ? "public, max-age=31536000, immutable" : "no-cache");
        sec_headers(req, strncmp(f[i].ctype, "text/html", 9) == 0);
        if (strcmp(f[i].path, "/index.html") == 0) evlog_mark(EVM_GUI_SERVE, (int32_t)f[i].len);
        return httpd_resp_send(req, (const char *)f[i].start, (ssize_t)f[i].len);   /* flash'tan tek seferde */
    }
    return httpd_resp_send_404(req);
}

static esp_err_t dispatch(httpd_req_t *req)
{
    http_req_t *r = &s_ctx;
    memset(r, 0, sizeof *r);
    r->req = req;
    fill_ip(r);
    if (strncmp(req->uri, "/api/", 5) != 0) {
        if (req->method != HTTP_GET) return http_fail(r, 405, "method", "", NULL);
        return serve_static(req);
    }
    const http_route_t *hit = NULL;
    bool path_ok = false;
    for (size_t i = 0; i < s_nroutes && !hit; i++) {
        if (!match(s_routes[i].pattern, req->uri, r)) continue;
        path_ok = true;
        if (s_routes[i].method == (httpd_method_t)req->method) hit = &s_routes[i];
    }
    if (!hit) return path_ok ? http_fail(r, 405, "method", "", NULL) : http_fail(r, 404, "not_found", "", NULL);
    if ((hit->flags & HR_AUTH) && http_auth_enabled() && !http_auth_session_valid(r)) {
        return http_fail(r, 401, "auth_required", "", NULL);
    }
    if (hit->flags & HR_CSRF) {
        const char *why = csrf_reject(r);
        if (why) return http_fail(r, 403, "forbidden", why, NULL);          /* işleyici ÇAĞRILMAZ */
    }
    if (hit->flags & HR_BODY) {
        body_res_t b = read_body(r);
        if (b == BODY_SOCKET_ERR) return ESP_FAIL;
        if (b == BODY_REJECTED) return ESP_OK;                               /* işleyici ÇAĞRILMAZ */
    }
    esp_err_t e = hit->fn(r);
    cJSON_Delete(r->body);
    r->body = NULL;
    return e;
}

/* ---------- kurulum ---------- */

esp_err_t http_start(const http_route_t *routes, size_t n)
{
    s_routes = routes;
    s_nroutes = n;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 8192;
    c.max_open_sockets = 10;
    c.max_resp_headers = 12;                               /* güvenlik başlıkları + çerez */
    c.max_uri_handlers = 8;
    c.max_resp_headers = 10;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 2;                               /* yarım istek (slowloris) tek httpd görevini kısa tutsun */
    c.send_wait_timeout = 5;
    c.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t r = httpd_start(&s_server, &c);
    if (r != ESP_OK) return r;
    static const httpd_method_t methods[] = { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE };
    for (size_t i = 0; i < 4; i++) {
        httpd_uri_t u = { .uri = "/*", .method = methods[i], .handler = dispatch };
        httpd_register_uri_handler(s_server, &u);
    }
    http_auth_init();
    http_sse_init(s_server);
    ESP_LOGI(TAG, "sunucu hazir (%u rota)", (unsigned)n);
    return ESP_OK;
}
