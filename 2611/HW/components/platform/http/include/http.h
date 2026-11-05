/*
 * http — HTTP sunucusu katmanı (04-plan-son.md §2.7, 03-arastirma/01).
 * - httpd.ye yöntem başına tek joker kayıt; eşleştirme, yol parametreleri, 404/405 bu katmanda.
 * - Tek istek bir anda işlenir → statik istek bağlamı (gövde, yanıt tamponu, çerez başlığı).
 * - Ön işleyici: oturum (HR_AUTH) → CSRF (HR_CSRF: Host beyaz listesi + X-LS-Request + Origin)
 *   → gövde (HR_BODY: application/json, ≤2 KB, aşım 413, bozuk 400).
 * - Yanıt zarfı: {"ok":true,"data":…} / {"ok":false,"error":{"code","message","field"?}}.
 * - Handler'lar ağ istemcisi çalıştırmaz, dosyaya yazmaz (uzun iş → sys_worker + jobs).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct cJSON cJSON;
typedef struct http_req http_req_t;
typedef esp_err_t (*http_handler_t)(http_req_t *r);

enum { HR_AUTH = 1, HR_CSRF = 2, HR_BODY = 4 };
#define HR_WRITE (HR_AUTH | HR_CSRF | HR_BODY)        /* durum değiştiren JSON uçları */

typedef struct {
    const char *pattern;          /* "/api/groups/{}/files/{}" — {} bir yol parçası */
    httpd_method_t method;
    uint8_t flags;
    http_handler_t fn;
} http_route_t;

esp_err_t    http_start(const http_route_t *routes, size_t n);
esp_err_t    http_ok(http_req_t *r, cJSON *data);                       /* data sahipliği alınır; NULL = {} */
esp_err_t    http_fail(http_req_t *r, int status, const char *code, const char *msg, const char *field);
const cJSON *http_body(http_req_t *r);                                  /* HR_BODY ile ayrıştırılmış */
const char  *http_param(http_req_t *r, int idx);                        /* {} parçaları, 0'dan */
int          http_param_int(http_req_t *r, int idx, int def);
const char  *http_query(http_req_t *r, const char *key, char *buf, size_t cap);
httpd_req_t *http_raw(http_req_t *r);                                   /* akış/dosya uçları için */
const char  *http_client_ip(http_req_t *r);
void         http_set_header(http_req_t *r, const char *name, const char *value);   /* değer kopyalanır */

/* kimlik (http_auth.c) */
bool      http_auth_enabled(void);                                      /* tarayıcı parolası ayarlı mı */
bool      http_auth_session_valid(http_req_t *r);
esp_err_t http_auth_login(http_req_t *r, const char *password);         /* ESP_ERR_INVALID_STATE = kilitli */
uint32_t  http_auth_locked_s(http_req_t *r);                            /* bu IP için kalan kilit */
void      http_auth_logout(http_req_t *r);
esp_err_t http_auth_set_password(const char *current, const char *next); /* next "" = kaldır */
void      http_auth_hash(const char *password, char out_hex[65]);       /* SHA-256(MAC:parola) */

/* SSE (http_sse.c): GET /api/events. Kaynak durum JSON'unu üretir; push her görevden güvenli. */
typedef cJSON *(*http_sse_source_t)(void);
void http_sse_set_source(http_sse_source_t fn);
void http_sse_broadcast(void);
esp_err_t http_sse_handler(http_req_t *r);

/* gömülü web arayüzü */
typedef struct {
    const char *path;             /* "/index.html", "/s/app.js", "/s/lang/tr.json" ... */
    const unsigned char *start;
    size_t len;
    const char *ctype;
} http_embedded_t;
const http_embedded_t *http_embedded_files(size_t *count, const char **hash);
