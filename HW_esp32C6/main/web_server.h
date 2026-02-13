/**
 * Web Server - HTTP Yonetimi (Token Auth ile)
 *
 * ESP-IDF httpd uzerinde calisan HTTP sunucu.
 * Bearer token + Cookie fallback ile kimlik dogrulama.
 * LittleFS'ten statik dosya servisi.
 * REST API: timer, mail, wifi, relay, ota, device.
 *
 * Bagimlilik: session_auth (Katman 2), tum manager'lar
 * Katman: 4 (Uygulama)
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sunucu ayarlari
#define WEB_SERVER_PORT         80
#define WEB_SERVER_MAX_URI      56
#define WEB_SERVER_STACK_SIZE   10240

/**
 * Web sunucuyu baslat
 */
esp_err_t web_server_start(void);

/**
 * Web sunucuyu durdur
 */
esp_err_t web_server_stop(void);

/**
 * Sunucu calisiyor mu
 */
bool web_server_is_running(void);

/**
 * JSON yanit gonder
 */
esp_err_t web_server_send_json(httpd_req_t *req, const char *json);

/**
 * Hata yaniti gonder
 */
esp_err_t web_server_send_error(httpd_req_t *req, int status_code, const char *message);

/**
 * LittleFS'ten dosya gonder (chunked)
 */
esp_err_t web_server_send_file(httpd_req_t *req, const char *filepath);

/**
 * Debug bilgileri
 */
void web_server_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
