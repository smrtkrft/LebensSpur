/**
 * Web Server Internal - API handler'lar icin paylasilan yardimcilar
 *
 * Bu header sadece api_*.c dosyalari tarafindan kullanilir.
 * Harici kullanim icin web_server.h'yi kullanin.
 */

#ifndef WEB_SERVER_INTERNAL_H
#define WEB_SERVER_INTERNAL_H

#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Request sayaci (web_server.c'de tanimli)
extern uint32_t ws_request_count;

// Bearer token + Cookie fallback ile session dogrulama
bool check_auth(httpd_req_t *req);

// 401 Unauthorized JSON yaniti
esp_err_t send_unauthorized(httpd_req_t *req);

// POST body oku (max_len'e kadar, null-terminate eder)
int read_body(httpd_req_t *req, char *buf, size_t max_len);

// Dosya uzantisindan MIME type belirle
const char *get_mime(const char *path);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_INTERNAL_H
