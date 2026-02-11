/**
 * @file web_server.c
 * @brief HTTP Web Server Implementation
 */

#include "web_server.h"
#include "session_auth.h"
#include "config_manager.h"
#include "file_manager.h"
#include "device_id.h"
#include "log_manager.h"
#include "time_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <sys/socket.h>

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;

/* ============================================
 * MIME TYPE LOOKUP
 * ============================================ */

static const char* get_mime_type(const char *filepath)
{
    const char *ext = strrchr(filepath, '.');
    if (!ext) return MIME_OCTET;
    
    ext++;  // Skip the dot
    
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return MIME_HTML;
    if (strcasecmp(ext, "css") == 0) return MIME_CSS;
    if (strcasecmp(ext, "js") == 0) return MIME_JS;
    if (strcasecmp(ext, "json") == 0) return MIME_JSON;
    if (strcasecmp(ext, "png") == 0) return MIME_PNG;
    if (strcasecmp(ext, "ico") == 0) return MIME_ICO;
    if (strcasecmp(ext, "svg") == 0) return MIME_SVG;
    
    return MIME_OCTET;
}

/* ============================================
 * HELPERS
 * ============================================ */

esp_err_t web_send_json(httpd_req_t *req, int status_code, const char *json)
{
    char status[32];
    snprintf(status, sizeof(status), "%d", status_code);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, MIME_JSON);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t web_send_error(httpd_req_t *req, int status_code, const char *error)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", error ? error : "Unknown error");
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    esp_err_t ret = web_send_json(req, status_code, json_str);
    free(json_str);
    return ret;
}

esp_err_t web_send_success(httpd_req_t *req, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    if (message) {
        cJSON_AddStringToObject(json, "message", message);
    }
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

esp_err_t web_send_file(httpd_req_t *req, const char *filepath)
{
    if (!file_manager_exists(filepath)) {
        return web_send_error(req, 404, "File not found");
    }
    
    int32_t file_size = file_manager_get_size(filepath);
    if (file_size <= 0) {
        return web_send_error(req, 500, "Cannot read file");
    }
    
    // Set content type
    httpd_resp_set_type(req, get_mime_type(filepath));
    
    // Set cache for static files
    if (strstr(filepath, ".html") == NULL) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    }
    
    // Read and send in chunks
    char *buffer = malloc(4096);
    if (!buffer) {
        return web_send_error(req, 500, "Memory error");
    }
    
    FILE *f = fopen(filepath, "r");
    if (!f) {
        free(buffer);
        return web_send_error(req, 500, "Cannot open file");
    }
    
    size_t read_bytes;
    do {
        read_bytes = fread(buffer, 1, 4096, f);
        if (read_bytes > 0) {
            httpd_resp_send_chunk(req, buffer, read_bytes);
        }
    } while (read_bytes == 4096);
    
    fclose(f);
    free(buffer);
    
    // End chunked response
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

void web_get_client_ip(httpd_req_t *req, char *ip_buffer)
{
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) == 0) {
        inet_ntoa_r(addr.sin_addr, ip_buffer, 16);
    } else {
        strcpy(ip_buffer, "unknown");
    }
}

int web_get_body(httpd_req_t *req, char *buffer, size_t max_len)
{
    int content_len = req->content_len;
    if (content_len <= 0) return 0;
    if (content_len > max_len - 1) content_len = max_len - 1;
    
    int received = httpd_req_recv(req, buffer, content_len);
    if (received > 0) {
        buffer[received] = '\0';
    }
    return received;
}

bool web_is_authenticated(httpd_req_t *req)
{
    char cookie[256] = "";
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    
    char token[SESSION_TOKEN_LEN + 1];
    if (session_auth_extract_token(cookie, token) != ESP_OK) {
        return false;
    }
    
    if (!session_auth_validate(token)) {
        return false;
    }
    
    // Refresh session
    session_auth_refresh(token);
    return true;
}

/* ============================================
 * API HANDLERS
 * ============================================ */

// POST /api/login
static esp_err_t api_login_handler(httpd_req_t *req)
{
    char body[256];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) {
        return web_send_error(req, 400, "No body");
    }
    
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        return web_send_error(req, 400, "Invalid JSON");
    }
    
    cJSON *pw_item = cJSON_GetObjectItem(json, "password");
    if (!pw_item || !cJSON_IsString(pw_item)) {
        cJSON_Delete(json);
        return web_send_error(req, 400, "Password required");
    }
    
    char ip[16];
    web_get_client_ip(req, ip);
    
    char user_agent[64] = "";
    httpd_req_get_hdr_value_str(req, "User-Agent", user_agent, sizeof(user_agent));
    
    char token[SESSION_TOKEN_LEN + 1];
    esp_err_t ret = session_auth_login(pw_item->valuestring, ip, user_agent, token);
    cJSON_Delete(json);
    
    if (ret == ESP_ERR_INVALID_STATE) {
        int remaining = session_auth_lockout_remaining();
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "success", false);
        cJSON_AddStringToObject(resp, "error", "Account locked");
        cJSON_AddNumberToObject(resp, "lockoutSeconds", remaining);
        char *resp_str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        esp_err_t r = web_send_json(req, 429, resp_str);
        free(resp_str);
        return r;
    }
    
    if (ret != ESP_OK) {
        int attempts = session_auth_remaining_attempts();
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "success", false);
        cJSON_AddStringToObject(resp, "error", "Invalid password");
        cJSON_AddNumberToObject(resp, "remainingAttempts", attempts);
        char *resp_str = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        esp_err_t r = web_send_json(req, 401, resp_str);
        free(resp_str);
        return r;
    }
    
    // Set cookie
    char cookie[128];
    session_auth_cookie_header(token, cookie, sizeof(cookie));
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    
    return web_send_success(req, "Login successful");
}

// POST /api/logout
static esp_err_t api_logout_handler(httpd_req_t *req)
{
    char cookie[256] = "";
    httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie));
    
    char token[SESSION_TOKEN_LEN + 1];
    if (session_auth_extract_token(cookie, token) == ESP_OK) {
        session_auth_logout(token);
    }
    
    // Clear cookie
    char clear_cookie[128];
    session_auth_logout_cookie(clear_cookie, sizeof(clear_cookie));
    httpd_resp_set_hdr(req, "Set-Cookie", clear_cookie);
    
    return web_send_success(req, "Logged out");
}

// GET /api/status
static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    
    // Device info
    cJSON_AddStringToObject(json, "deviceId", device_id_get());
    
    // Time
    char time_str[30];
    time_manager_get_iso8601(time_str, sizeof(time_str));
    cJSON_AddStringToObject(json, "time", time_str);
    cJSON_AddBoolToObject(json, "timeSynced", time_manager_is_synced());
    
    // Uptime
    char uptime_str[32];
    time_manager_get_uptime_string(uptime_str, sizeof(uptime_str));
    cJSON_AddStringToObject(json, "uptime", uptime_str);
    
    // Auth status
    cJSON_AddBoolToObject(json, "authenticated", web_is_authenticated(req));
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// GET /api/config/timer (requires auth)
static esp_err_t api_get_timer_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    timer_config_t config;
    config_load_timer(&config);
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", config.enabled);
    cJSON_AddNumberToObject(json, "intervalHours", config.interval_hours);
    cJSON_AddNumberToObject(json, "warningMinutes", config.warning_minutes);
    cJSON_AddNumberToObject(json, "alarmCount", config.alarm_count);
    cJSON_AddStringToObject(json, "checkStart", config.check_start);
    cJSON_AddStringToObject(json, "checkEnd", config.check_end);
    cJSON_AddBoolToObject(json, "relayTrigger", config.relay_trigger);
    cJSON_AddBoolToObject(json, "vacationEnabled", config.vacation_enabled);
    cJSON_AddNumberToObject(json, "vacationDays", config.vacation_days);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// POST /api/config/timer (requires auth)
static esp_err_t api_set_timer_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[512];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) {
        return web_send_error(req, 400, "No body");
    }
    
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        return web_send_error(req, 400, "Invalid JSON");
    }
    
    timer_config_t config;
    config_load_timer(&config);
    
    // Update fields if present
    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "enabled")) && cJSON_IsBool(item)) {
        config.enabled = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, "intervalHours")) && cJSON_IsNumber(item)) {
        config.interval_hours = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "warningMinutes")) && cJSON_IsNumber(item)) {
        config.warning_minutes = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "alarmCount")) && cJSON_IsNumber(item)) {
        config.alarm_count = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "checkStart")) && cJSON_IsString(item)) {
        strncpy(config.check_start, item->valuestring, sizeof(config.check_start) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "checkEnd")) && cJSON_IsString(item)) {
        strncpy(config.check_end, item->valuestring, sizeof(config.check_end) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "relayTrigger")) && cJSON_IsBool(item)) {
        config.relay_trigger = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, "vacationEnabled")) && cJSON_IsBool(item)) {
        config.vacation_enabled = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, "vacationDays")) && cJSON_IsNumber(item)) {
        config.vacation_days = item->valueint;
    }
    
    cJSON_Delete(json);
    
    config_save_timer(&config);
    LOG_CONFIG(LOG_LEVEL_INFO, "Timer config updated");
    
    return web_send_success(req, "Timer config saved");
}

// POST /api/timer/reset (requires auth)
static esp_err_t api_timer_reset(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    // This will be implemented in timer_scheduler
    LOG_TIMER(LOG_LEVEL_INFO, "Timer reset via web");
    return web_send_success(req, "Timer reset");
}

// GET /api/logs (requires auth)
static esp_err_t api_get_logs(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    log_filter_t filter = LOG_FILTER_DEFAULT();
    filter.max_entries = 200;
    
    // Allocate buffer for logs
    char *buffer = malloc(16384);
    if (!buffer) {
        return web_send_error(req, 500, "Memory error");
    }
    
    size_t len = log_get_entries_json(&filter, buffer, 16384);
    
    httpd_resp_set_type(req, MIME_JSON);
    httpd_resp_send(req, buffer, len);
    free(buffer);
    
    return ESP_OK;
}

// DELETE /api/logs (requires auth)
static esp_err_t api_clear_logs(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    log_clear();
    return web_send_success(req, "Logs cleared");
}

/* ============================================
 * STATIC FILE HANDLER
 * ============================================ */

static esp_err_t static_file_handler(httpd_req_t *req)
{
    char filepath[128];
    const char *uri = req->uri;
    
    // Default to index.html
    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }
    
    // Build filepath
    snprintf(filepath, sizeof(filepath), "%s%s", WEB_STATIC_DIR, uri);
    
    // Security: prevent path traversal
    if (strstr(filepath, "..") != NULL) {
        return web_send_error(req, 403, "Forbidden");
    }
    
    return web_send_file(req, filepath);
}

/* ============================================
 * ROUTE REGISTRATION
 * ============================================ */

void web_register_api_routes(httpd_handle_t server)
{
    // Auth routes
    httpd_uri_t login = {
        .uri = "/api/login",
        .method = HTTP_POST,
        .handler = api_login_handler
    };
    httpd_register_uri_handler(server, &login);
    
    httpd_uri_t logout = {
        .uri = "/api/logout",
        .method = HTTP_POST,
        .handler = api_logout_handler
    };
    httpd_register_uri_handler(server, &logout);
    
    // Status
    httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler
    };
    httpd_register_uri_handler(server, &status);
    
    // Timer config
    httpd_uri_t timer_get = {
        .uri = "/api/config/timer",
        .method = HTTP_GET,
        .handler = api_get_timer_config
    };
    httpd_register_uri_handler(server, &timer_get);
    
    httpd_uri_t timer_set = {
        .uri = "/api/config/timer",
        .method = HTTP_POST,
        .handler = api_set_timer_config
    };
    httpd_register_uri_handler(server, &timer_set);
    
    // Timer reset
    httpd_uri_t timer_reset = {
        .uri = "/api/timer/reset",
        .method = HTTP_POST,
        .handler = api_timer_reset
    };
    httpd_register_uri_handler(server, &timer_reset);
    
    // Logs
    httpd_uri_t logs_get = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_get_logs
    };
    httpd_register_uri_handler(server, &logs_get);
    
    httpd_uri_t logs_clear = {
        .uri = "/api/logs",
        .method = HTTP_DELETE,
        .handler = api_clear_logs
    };
    httpd_register_uri_handler(server, &logs_clear);
    
    ESP_LOGI(TAG, "API routes registered");
}

void web_register_static_routes(httpd_handle_t server)
{
    // Catch-all for static files
    httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler
    };
    httpd_register_uri_handler(server, &static_uri);
    
    ESP_LOGI(TAG, "Static file handler registered");
}

/* ============================================
 * SERVER INIT/STOP
 * ============================================ */

esp_err_t web_server_init(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting web server on port %d...", WEB_SERVER_PORT);
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = WEB_MAX_HANDLERS;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register routes
    web_register_api_routes(s_server);
    web_register_static_routes(s_server);
    
    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

bool web_server_is_running(void)
{
    return s_server != NULL;
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}
