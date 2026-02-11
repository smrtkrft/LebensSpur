/**
 * @file web_server.h
 * @brief HTTP Web Server
 * 
 * Provides:
 * - Static file serving from SPIFFS
 * - REST API endpoints
 * - Session authentication middleware
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * CONFIGURATION
 * ============================================ */
#define WEB_SERVER_PORT         80
#define WEB_MAX_URI_LEN         128
#define WEB_MAX_HANDLERS        32
#define WEB_STATIC_DIR          "/ext/web"

// MIME types
#define MIME_HTML       "text/html"
#define MIME_CSS        "text/css"
#define MIME_JS         "application/javascript"
#define MIME_JSON       "application/json"
#define MIME_PNG        "image/png"
#define MIME_ICO        "image/x-icon"
#define MIME_SVG        "image/svg+xml"
#define MIME_OCTET      "application/octet-stream"

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize and start web server
 */
esp_err_t web_server_init(void);

/**
 * @brief Stop web server
 */
void web_server_stop(void);

/**
 * @brief Check if server is running
 */
bool web_server_is_running(void);

/**
 * @brief Get server handle
 */
httpd_handle_t web_server_get_handle(void);

/* ============================================
 * API HELPERS
 * ============================================ */

/**
 * @brief Send JSON response
 * @param req HTTP request
 * @param status_code HTTP status
 * @param json JSON string
 */
esp_err_t web_send_json(httpd_req_t *req, int status_code, const char *json);

/**
 * @brief Send JSON error response
 * @param req HTTP request
 * @param status_code HTTP status
 * @param error Error message
 */
esp_err_t web_send_error(httpd_req_t *req, int status_code, const char *error);

/**
 * @brief Send JSON success response
 * @param req HTTP request
 * @param message Optional message (can be NULL)
 */
esp_err_t web_send_success(httpd_req_t *req, const char *message);

/**
 * @brief Send file from SPIFFS
 * @param req HTTP request
 * @param filepath Path on SPIFFS
 */
esp_err_t web_send_file(httpd_req_t *req, const char *filepath);

/* ============================================
 * AUTH MIDDLEWARE
 * ============================================ */

/**
 * @brief Check if request is authenticated
 * @param req HTTP request
 * @return true if valid session
 */
bool web_is_authenticated(httpd_req_t *req);

/**
 * @brief Get client IP from request
 * @param req HTTP request
 * @param ip_buffer Output buffer (min 16 chars)
 */
void web_get_client_ip(httpd_req_t *req, char *ip_buffer);

/**
 * @brief Get request body as string
 * @param req HTTP request
 * @param buffer Output buffer
 * @param max_len Maximum length
 * @return Number of bytes read
 */
int web_get_body(httpd_req_t *req, char *buffer, size_t max_len);

/* ============================================
 * ROUTE REGISTRATION
 * ============================================ */

/**
 * @brief Register API routes
 * Called internally by web_server_init
 */
void web_register_api_routes(httpd_handle_t server);

/**
 * @brief Register static file handler
 */
void web_register_static_routes(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
