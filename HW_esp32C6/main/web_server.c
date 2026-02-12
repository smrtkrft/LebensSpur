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
#include "wifi_manager.h"
#include "timer_scheduler.h"
#include "relay_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "ext_flash.h"

static const char *TAG = "web_server";

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.0"
#endif

static httpd_handle_t s_server = NULL;

/* Embedded setup.html - available without SPIFFS */
extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_end");

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
    cJSON_AddNumberToObject(json, "intervalMinutes", config.interval_minutes);
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
    if ((item = cJSON_GetObjectItem(json, "intervalMinutes")) && cJSON_IsNumber(item)) {
        config.interval_minutes = item->valueint;
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
    
    esp_err_t ret = timer_scheduler_reset();
    if (ret != ESP_OK) {
        return web_send_error(req, 400, "Timer cannot be reset (disabled or triggered)");
    }
    
    LOG_TIMER(LOG_LEVEL_INFO, "Timer reset via web");
    return web_send_success(req, "Timer reset");
}

// GET /api/timer/status (requires auth)
static esp_err_t api_timer_status(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    timer_status_t status;
    timer_scheduler_get_status(&status);
    
    timer_config_t config;
    config_load_timer(&config);
    
    cJSON *json = cJSON_CreateObject();
    
    // State
    cJSON_AddStringToObject(json, "state", timer_scheduler_state_name(status.state));
    cJSON_AddNumberToObject(json, "stateCode", (int)status.state);
    
    // Timing
    cJSON_AddNumberToObject(json, "timeRemainingMs", (double)status.time_remaining_ms);
    cJSON_AddNumberToObject(json, "intervalMinutes", config.interval_minutes);
    cJSON_AddBoolToObject(json, "inTimeWindow", status.in_time_window);
    
    // Counters
    cJSON_AddNumberToObject(json, "warningsSent", status.warnings_sent);
    cJSON_AddNumberToObject(json, "resetCount", status.reset_count);
    cJSON_AddNumberToObject(json, "triggerCount", status.trigger_count);
    cJSON_AddNumberToObject(json, "alarmCount", config.alarm_count);
    
    // Config summary
    cJSON_AddBoolToObject(json, "enabled", config.enabled);
    cJSON_AddBoolToObject(json, "vacationEnabled", config.vacation_enabled);
    cJSON_AddNumberToObject(json, "vacationDays", config.vacation_days);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// POST /api/timer/enable (requires auth)
static esp_err_t api_timer_enable(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    timer_scheduler_enable();
    return web_send_success(req, "Timer enabled");
}

// POST /api/timer/disable (requires auth)
static esp_err_t api_timer_disable(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    timer_scheduler_disable();
    return web_send_success(req, "Timer disabled");
}

// POST /api/timer/acknowledge (requires auth)
static esp_err_t api_timer_acknowledge(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    esp_err_t ret = timer_scheduler_acknowledge();
    if (ret != ESP_OK) {
        return web_send_error(req, 400, "Timer not triggered");
    }
    
    relay_off();
    LOG_TIMER(LOG_LEVEL_INFO, "Trigger acknowledged via web, relay off");
    return web_send_success(req, "Acknowledged");
}

// POST /api/timer/vacation (requires auth) - body: {"enabled":true,"days":7}
static esp_err_t api_timer_vacation(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[256];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) {
        return web_send_error(req, 400, "No body");
    }
    
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        return web_send_error(req, 400, "Invalid JSON");
    }
    
    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (enabled && cJSON_IsTrue(enabled)) {
        cJSON *days = cJSON_GetObjectItem(json, "days");
        int d = (days && cJSON_IsNumber(days)) ? days->valueint : 7;
        timer_scheduler_vacation_start(d);
    } else {
        timer_scheduler_vacation_end();
    }
    
    cJSON_Delete(json);
    return web_send_success(req, "Vacation mode updated");
}

// POST /api/relay/test (requires auth)  
static esp_err_t api_relay_test(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    relay_config_t config;
    config_load_relay(&config);
    
    if (config.pulse_mode) {
        relay_pulse(config.pulse_duration_ms > 0 ? config.pulse_duration_ms : 1000);
    } else {
        relay_pulse(1000); // Default 1 second test pulse
    }
    
    LOG_SYSTEM(LOG_LEVEL_INFO, "Relay test via web");
    return web_send_success(req, "Relay test pulse sent");
}

// GET /api/config/relay (requires auth)
static esp_err_t api_get_relay_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    relay_config_t config;
    config_load_relay(&config);
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "inverted", config.inverted);
    cJSON_AddBoolToObject(json, "pulseMode", config.pulse_mode);
    cJSON_AddNumberToObject(json, "pulseDurationMs", config.pulse_duration_ms);
    cJSON_AddNumberToObject(json, "pulseIntervalMs", config.pulse_interval_ms);
    cJSON_AddNumberToObject(json, "pulseCount", config.pulse_count);
    cJSON_AddNumberToObject(json, "onDelayMs", config.on_delay_ms);
    cJSON_AddNumberToObject(json, "offDelayMs", config.off_delay_ms);
    cJSON_AddBoolToObject(json, "relayOn", relay_is_on());
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// POST /api/config/relay (requires auth)
static esp_err_t api_set_relay_config(httpd_req_t *req)
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
    
    relay_config_t config;
    config_load_relay(&config);
    
    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "inverted")) && cJSON_IsBool(item)) {
        config.inverted = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, "pulseMode")) && cJSON_IsBool(item)) {
        config.pulse_mode = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, "pulseDurationMs")) && cJSON_IsNumber(item)) {
        config.pulse_duration_ms = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "pulseIntervalMs")) && cJSON_IsNumber(item)) {
        config.pulse_interval_ms = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "pulseCount")) && cJSON_IsNumber(item)) {
        config.pulse_count = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "onDelayMs")) && cJSON_IsNumber(item)) {
        config.on_delay_ms = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "offDelayMs")) && cJSON_IsNumber(item)) {
        config.off_delay_ms = item->valueint;
    }
    
    cJSON_Delete(json);
    
    config_save_relay(&config);
    LOG_CONFIG(LOG_LEVEL_INFO, "Relay config updated");
    
    return web_send_success(req, "Relay config saved");
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
 * EMBEDDED SETUP PAGE HANDLER
 * ============================================ */

static esp_err_t setup_html_handler(httpd_req_t *req)
{
    size_t setup_html_len = setup_html_end - setup_html_start;
    httpd_resp_set_type(req, MIME_HTML);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)setup_html_start, setup_html_len);
}

/* ============================================
 * SETUP API HANDLERS (no auth required)
 * ============================================ */

// GET /api/setup/wifi/scan
static esp_err_t api_setup_wifi_scan(httpd_req_t *req)
{
    ESP_LOGI(TAG, "WiFi scan requested");
    
    wifi_scan_result_t results[WIFI_SCAN_MAX_AP];
    int count = wifi_manager_scan(results, WIFI_SCAN_MAX_AP);
    
    cJSON *json = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", results[i].ssid);
            cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
            cJSON_AddNumberToObject(net, "auth", results[i].authmode);
            cJSON_AddItemToArray(networks, net);
        }
    }
    
    cJSON_AddItemToObject(json, "networks", networks);
    cJSON_AddBoolToObject(json, "success", true);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// POST /api/setup/wifi/connect
static esp_err_t api_setup_wifi_connect(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return web_send_error(req, 400, "No data received");
    }
    buf[ret] = '\0';
    
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        return web_send_error(req, 400, "Invalid JSON");
    }
    
    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(json, "password");
    
    if (!ssid_item || !cJSON_IsString(ssid_item)) {
        cJSON_Delete(json);
        return web_send_error(req, 400, "Missing SSID");
    }
    
    const char *ssid = ssid_item->valuestring;
    const char *password = (pass_item && cJSON_IsString(pass_item)) ? pass_item->valuestring : "";
    
    ESP_LOGI(TAG, "WiFi connect requested: SSID=%s", ssid);
    
    // Start connection (non-blocking - client will poll for status)
    // Save config first
    app_wifi_config_t wifi_cfg = WIFI_CONFIG_DEFAULT();
    strncpy(wifi_cfg.ssid, ssid, sizeof(wifi_cfg.ssid) - 1);
    strncpy(wifi_cfg.password, password, sizeof(wifi_cfg.password) - 1);
    wifi_cfg.configured = true;
    config_save_wifi(&wifi_cfg);
    
    cJSON_Delete(json);
    
    // Start connection in background - response sent before channel change
    esp_err_t err = wifi_manager_connect_async(ssid, password);
    
    if (err == ESP_OK) {
        return web_send_success(req, "Connecting");
    } else {
        return web_send_error(req, 500, "Failed to start connection");
    }
}

// POST /api/setup/password
static esp_err_t api_setup_password(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        return web_send_error(req, 400, "No data received");
    }
    buf[ret] = '\0';
    
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        return web_send_error(req, 400, "Invalid JSON");
    }
    
    cJSON *pass_item = cJSON_GetObjectItem(json, "password");
    if (!pass_item || !cJSON_IsString(pass_item)) {
        cJSON_Delete(json);
        return web_send_error(req, 400, "Missing password");
    }
    
    const char *password = pass_item->valuestring;
    ESP_LOGI(TAG, "Setting device password");
    
    auth_config_t auth_cfg = AUTH_CONFIG_DEFAULT();
    strncpy(auth_cfg.password, password, sizeof(auth_cfg.password) - 1);
    
    esp_err_t err = config_save_auth(&auth_cfg);
    cJSON_Delete(json);
    
    if (err != ESP_OK) {
        return web_send_error(req, 500, "Failed to save password");
    }
    
    return web_send_success(req, "Password saved");
}

// POST /api/setup/complete
static esp_err_t api_setup_complete(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Setup complete requested");
    
    config_mark_setup_completed();
    
    LOG_SYSTEM(LOG_LEVEL_INFO, "Initial setup completed");
    
    return web_send_success(req, "Setup complete");
}

// GET /api/wifi/status - WiFi connection status for setup
static esp_err_t api_wifi_status(httpd_req_t *req)
{
    wifi_status_t status;
    wifi_manager_get_status(&status);
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "connected", status.sta_connected);
    cJSON_AddStringToObject(json, "ssid", status.sta_ssid);
    cJSON_AddStringToObject(json, "ip", status.sta_ip);
    cJSON_AddNumberToObject(json, "rssi", status.sta_rssi);
    cJSON_AddBoolToObject(json, "ap_active", status.ap_active);
    cJSON_AddStringToObject(json, "ap_ip", status.ap_ip);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// GET /api/device/info - Device information (comprehensive)
static esp_err_t api_device_info(httpd_req_t *req)
{
    // Chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const char *chip_model_str = "Unknown";
    switch (chip_info.model) {
        case CHIP_ESP32:   chip_model_str = "ESP32"; break;
        case CHIP_ESP32S2: chip_model_str = "ESP32-S2"; break;
        case CHIP_ESP32S3: chip_model_str = "ESP32-S3"; break;
        case CHIP_ESP32C3: chip_model_str = "ESP32-C3"; break;
        case CHIP_ESP32C6: chip_model_str = "ESP32-C6"; break;
        case CHIP_ESP32H2: chip_model_str = "ESP32-H2"; break;
        default: break;
    }

    // MAC address
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // WiFi status
    wifi_status_t wifi_status;
    wifi_manager_get_status(&wifi_status);

    // Heap info
    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t heap_min_free = esp_get_minimum_free_heap_size();
    uint32_t heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);

    // Internal flash
    uint32_t int_flash_total = 0;
    esp_flash_get_size(NULL, &int_flash_total);

    // Partition sizes
    const esp_partition_t *app_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    const esp_partition_t *nvs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);

    // External flash
    uint32_t ext_flash_total = ext_flash_get_size();
    uint32_t ext_spiffs_total = 0, ext_spiffs_used = 0;
    file_manager_get_info(&ext_spiffs_total, &ext_spiffs_used);

    // Uptime
    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_s = (uint32_t)(uptime_us / 1000000);

    // Reset reason
    esp_reset_reason_t reset_reason = esp_reset_reason();

    // NTP & time
    bool ntp_synced = time_manager_is_synced();
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%d.%m.%Y %H:%M:%S", &timeinfo);

    // CPU frequency
    int cpu_freq = 160;
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
    cpu_freq = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#endif

    // Build JSON
    cJSON *json = cJSON_CreateObject();

    // Identity
    cJSON_AddStringToObject(json, "device_id", device_id_get());
    cJSON_AddStringToObject(json, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(json, "mac", mac_str);
    cJSON_AddStringToObject(json, "hostname", wifi_manager_get_hostname());

    // Hardware
    cJSON_AddStringToObject(json, "chip_model", chip_model_str);
    cJSON_AddNumberToObject(json, "chip_cores", chip_info.cores);
    cJSON_AddNumberToObject(json, "chip_revision", chip_info.revision);
    cJSON_AddNumberToObject(json, "cpu_freq_mhz", cpu_freq);
    cJSON_AddNumberToObject(json, "int_flash_total", int_flash_total);

    // Heap
    cJSON_AddNumberToObject(json, "heap_total", heap_total);
    cJSON_AddNumberToObject(json, "heap_free", heap_free);
    cJSON_AddNumberToObject(json, "heap_min_free", heap_min_free);

    // Partitions
    cJSON_AddNumberToObject(json, "app_size", app_part ? app_part->size : 0);
    cJSON_AddNumberToObject(json, "ota_size", ota_part ? ota_part->size : 0);
    cJSON_AddNumberToObject(json, "nvs_size", nvs_part ? nvs_part->size : 0);

    // External flash
    cJSON_AddNumberToObject(json, "ext_flash_total", ext_flash_total);
    cJSON_AddNumberToObject(json, "ext_spiffs_total", ext_spiffs_total);
    cJSON_AddNumberToObject(json, "ext_spiffs_used", ext_spiffs_used);

    // WiFi
    cJSON_AddBoolToObject(json, "sta_connected", wifi_status.sta_connected);
    cJSON_AddStringToObject(json, "sta_ssid", wifi_status.sta_ssid);
    cJSON_AddStringToObject(json, "sta_ip", wifi_status.sta_ip);
    cJSON_AddNumberToObject(json, "sta_rssi", wifi_status.sta_rssi);
    cJSON_AddBoolToObject(json, "ap_active", wifi_status.ap_active);
    cJSON_AddStringToObject(json, "ap_ip", wifi_status.ap_ip);

    // Runtime
    cJSON_AddNumberToObject(json, "uptime_s", uptime_s);
    cJSON_AddNumberToObject(json, "reset_reason", (int)reset_reason);
    cJSON_AddBoolToObject(json, "ntp_synced", ntp_synced);
    cJSON_AddStringToObject(json, "time", time_str);

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

/* ============================================
 * WIFI CONFIG
 * ============================================ */

// GET /api/config/wifi (requires auth)
static esp_err_t api_get_wifi_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    app_wifi_config_t primary_cfg;
    app_wifi_config_t backup_cfg;
    config_load_wifi(&primary_cfg);
    config_load_wifi_backup(&backup_cfg);
    
    cJSON *json = cJSON_CreateObject();
    
    // Primary WiFi
    cJSON *primary = cJSON_CreateObject();
    cJSON_AddStringToObject(primary, "ssid", primary_cfg.ssid);
    cJSON_AddStringToObject(primary, "password", primary_cfg.password);
    cJSON_AddBoolToObject(primary, "configured", primary_cfg.configured);
    cJSON_AddBoolToObject(primary, "staticIpEnabled", primary_cfg.static_ip_enabled);
    cJSON_AddStringToObject(primary, "staticIp", primary_cfg.static_ip);
    cJSON_AddStringToObject(primary, "gateway", primary_cfg.gateway);
    cJSON_AddStringToObject(primary, "subnet", primary_cfg.subnet);
    cJSON_AddStringToObject(primary, "dns", primary_cfg.dns);
    cJSON_AddItemToObject(json, "primary", primary);
    
    // Backup WiFi
    cJSON *backup = cJSON_CreateObject();
    cJSON_AddStringToObject(backup, "ssid", backup_cfg.ssid);
    cJSON_AddStringToObject(backup, "password", backup_cfg.password);
    cJSON_AddBoolToObject(backup, "configured", backup_cfg.configured);
    cJSON_AddBoolToObject(backup, "staticIpEnabled", backup_cfg.static_ip_enabled);
    cJSON_AddStringToObject(backup, "staticIp", backup_cfg.static_ip);
    cJSON_AddStringToObject(backup, "gateway", backup_cfg.gateway);
    cJSON_AddStringToObject(backup, "subnet", backup_cfg.subnet);
    cJSON_AddStringToObject(backup, "dns", backup_cfg.dns);
    cJSON_AddItemToObject(json, "backup", backup);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// POST /api/config/wifi (requires auth)
static esp_err_t api_set_wifi_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[512];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    cJSON *json = cJSON_Parse(body);
    if (!json) return web_send_error(req, 400, "Invalid JSON");
    
    // Determine if this is primary or backup
    cJSON *type_item = cJSON_GetObjectItem(json, "type");
    bool is_backup = (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "backup") == 0);
    
    app_wifi_config_t cfg = WIFI_CONFIG_DEFAULT();
    
    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass = cJSON_GetObjectItem(json, "password");
    cJSON *static_en = cJSON_GetObjectItem(json, "staticIpEnabled");
    cJSON *static_ip = cJSON_GetObjectItem(json, "staticIp");
    cJSON *gw = cJSON_GetObjectItem(json, "gateway");
    cJSON *subnet = cJSON_GetObjectItem(json, "subnet");
    cJSON *dns = cJSON_GetObjectItem(json, "dns");
    
    if (ssid && cJSON_IsString(ssid))
        strncpy(cfg.ssid, ssid->valuestring, MAX_SSID_LEN);
    if (pass && cJSON_IsString(pass))
        strncpy(cfg.password, pass->valuestring, MAX_PASSWORD_LEN);
    cfg.configured = (strlen(cfg.ssid) > 0);
    
    if (static_en && cJSON_IsBool(static_en))
        cfg.static_ip_enabled = cJSON_IsTrue(static_en);
    if (static_ip && cJSON_IsString(static_ip))
        strncpy(cfg.static_ip, static_ip->valuestring, sizeof(cfg.static_ip) - 1);
    if (gw && cJSON_IsString(gw))
        strncpy(cfg.gateway, gw->valuestring, sizeof(cfg.gateway) - 1);
    if (subnet && cJSON_IsString(subnet))
        strncpy(cfg.subnet, subnet->valuestring, sizeof(cfg.subnet) - 1);
    if (dns && cJSON_IsString(dns))
        strncpy(cfg.dns, dns->valuestring, sizeof(cfg.dns) - 1);
    
    esp_err_t err;
    if (is_backup) {
        err = config_save_wifi_backup(&cfg);
        LOG_CONFIG(LOG_LEVEL_INFO, "Backup WiFi config saved: %s", cfg.ssid);
    } else {
        err = config_save_wifi(&cfg);
        LOG_CONFIG(LOG_LEVEL_INFO, "Primary WiFi config saved: %s", cfg.ssid);
    }
    
    cJSON_Delete(json);
    
    if (err != ESP_OK) {
        return web_send_error(req, 500, "Failed to save WiFi config");
    }
    
    return web_send_success(req, is_backup ? "Backup WiFi saved" : "Primary WiFi saved");
}

/* ============================================
 * SMTP CONFIG
 * ============================================ */

// GET /api/config/smtp (requires auth)
static esp_err_t api_get_smtp_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "smtpServer", mail_cfg.smtp_server);
    cJSON_AddNumberToObject(json, "smtpPort", mail_cfg.smtp_port);
    cJSON_AddStringToObject(json, "smtpUsername", mail_cfg.smtp_username);
    cJSON_AddStringToObject(json, "smtpPassword", mail_cfg.smtp_password);
    cJSON_AddStringToObject(json, "senderName", mail_cfg.sender_name);
    cJSON_AddBoolToObject(json, "useTls", mail_cfg.use_tls);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// POST /api/config/smtp (requires auth)
static esp_err_t api_set_smtp_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[512];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    cJSON *json = cJSON_Parse(body);
    if (!json) return web_send_error(req, 400, "Invalid JSON");
    
    // Load existing mail config (preserves groups)
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    cJSON *server = cJSON_GetObjectItem(json, "smtpServer");
    cJSON *port = cJSON_GetObjectItem(json, "smtpPort");
    cJSON *username = cJSON_GetObjectItem(json, "smtpUsername");
    cJSON *password = cJSON_GetObjectItem(json, "smtpPassword");
    cJSON *sender = cJSON_GetObjectItem(json, "senderName");
    cJSON *tls = cJSON_GetObjectItem(json, "useTls");
    
    if (server && cJSON_IsString(server))
        strncpy(mail_cfg.smtp_server, server->valuestring, MAX_HOSTNAME_LEN);
    if (port && cJSON_IsNumber(port))
        mail_cfg.smtp_port = (uint16_t)port->valueint;
    if (username && cJSON_IsString(username))
        strncpy(mail_cfg.smtp_username, username->valuestring, MAX_EMAIL_LEN);
    if (password && cJSON_IsString(password))
        strncpy(mail_cfg.smtp_password, password->valuestring, MAX_PASSWORD_LEN);
    if (sender && cJSON_IsString(sender))
        strncpy(mail_cfg.sender_name, sender->valuestring, MAX_GROUP_NAME_LEN);
    if (tls && cJSON_IsBool(tls))
        mail_cfg.use_tls = cJSON_IsTrue(tls);
    
    cJSON_Delete(json);
    
    esp_err_t err = config_save_mail(&mail_cfg);
    if (err != ESP_OK) {
        return web_send_error(req, 500, "Failed to save SMTP config");
    }
    
    LOG_CONFIG(LOG_LEVEL_INFO, "SMTP config saved: %s:%d", mail_cfg.smtp_server, mail_cfg.smtp_port);
    return web_send_success(req, "SMTP config saved");
}

/* ============================================
 * AP MODE CONFIG
 * ============================================ */

// POST /api/config/ap (requires auth)
static esp_err_t api_set_ap_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[256];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    cJSON *json = cJSON_Parse(body);
    if (!json) return web_send_error(req, 400, "Invalid JSON");
    
    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (!enabled || !cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        return web_send_error(req, 400, "Missing 'enabled' field");
    }
    
    bool ap_enabled = cJSON_IsTrue(enabled);
    
    // Safety check: don't allow disabling AP if WiFi is not configured
    if (!ap_enabled) {
        app_wifi_config_t wifi_cfg;
        config_load_wifi(&wifi_cfg);
        if (!wifi_cfg.configured || strlen(wifi_cfg.ssid) == 0) {
            cJSON_Delete(json);
            return web_send_error(req, 400, "Cannot disable AP mode without configured WiFi");
        }
    }
    
    cJSON_Delete(json);
    
    // Apply AP mode change
    if (ap_enabled) {
        wifi_manager_start_ap();
        LOG_CONFIG(LOG_LEVEL_INFO, "AP mode enabled");
    } else {
        wifi_manager_stop_ap();
        LOG_CONFIG(LOG_LEVEL_WARN, "AP mode disabled");
    }
    
    return web_send_success(req, ap_enabled ? "AP mode enabled" : "AP mode disabled");
}

/* ============================================
 * PASSWORD CHANGE
 * ============================================ */

// POST /api/config/password (requires auth)
static esp_err_t api_change_password(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[256];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    cJSON *json = cJSON_Parse(body);
    if (!json) return web_send_error(req, 400, "Invalid JSON");
    
    cJSON *current = cJSON_GetObjectItem(json, "currentPassword");
    cJSON *newpw = cJSON_GetObjectItem(json, "newPassword");
    
    if (!current || !cJSON_IsString(current) || !newpw || !cJSON_IsString(newpw)) {
        cJSON_Delete(json);
        return web_send_error(req, 400, "Missing fields");
    }
    
    if (!config_verify_password(current->valuestring)) {
        cJSON_Delete(json);
        return web_send_error(req, 403, "Current password incorrect");
    }
    
    auth_config_t auth;
    config_load_auth(&auth);
    strncpy(auth.password, newpw->valuestring, MAX_PASSWORD_LEN);
    auth.password[MAX_PASSWORD_LEN] = '\0';
    config_save_auth(&auth);
    
    cJSON_Delete(json);
    LOG_CONFIG(LOG_LEVEL_INFO, "Password changed");
    return web_send_success(req, "Password changed");
}

/* ============================================
 * MAIL GROUPS CONFIG
 * ============================================ */

// GET /api/config/mail-groups (requires auth)
static esp_err_t api_get_mail_groups(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    cJSON *json = cJSON_CreateObject();
    cJSON *groups = cJSON_CreateArray();
    
    for (int i = 0; i < mail_cfg.group_count && i < MAX_MAIL_GROUPS; i++) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "name", mail_cfg.groups[i].name);
        cJSON_AddStringToObject(g, "subject", mail_cfg.groups[i].subject);
        cJSON_AddStringToObject(g, "content", mail_cfg.groups[i].body);
        cJSON *recips = cJSON_CreateArray();
        for (int j = 0; j < mail_cfg.groups[i].recipient_count && j < MAX_RECIPIENTS; j++) {
            cJSON_AddItemToArray(recips, cJSON_CreateString(mail_cfg.groups[i].recipients[j]));
        }
        cJSON_AddItemToObject(g, "recipients", recips);
        cJSON_AddItemToArray(groups, g);
    }
    
    cJSON_AddItemToObject(json, "groups", groups);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// POST /api/config/mail-groups (requires auth)
static esp_err_t api_set_mail_groups(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char *body = malloc(4096);
    if (!body) return web_send_error(req, 500, "Memory error");
    
    int len = web_get_body(req, body, 4096);
    if (len <= 0) { free(body); return web_send_error(req, 400, "No body"); }
    
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return web_send_error(req, 400, "Invalid JSON");
    
    mail_config_t mail_cfg;
    config_load_mail(&mail_cfg);
    
    cJSON *groups = cJSON_GetObjectItem(json, "groups");
    if (groups && cJSON_IsArray(groups)) {
        int count = cJSON_GetArraySize(groups);
        if (count > MAX_MAIL_GROUPS) count = MAX_MAIL_GROUPS;
        mail_cfg.group_count = count;
        
        for (int i = 0; i < count; i++) {
            cJSON *g = cJSON_GetArrayItem(groups, i);
            mail_group_t *mg = &mail_cfg.groups[i];
            
            cJSON *name = cJSON_GetObjectItem(g, "name");
            if (name && cJSON_IsString(name))
                strncpy(mg->name, name->valuestring, MAX_GROUP_NAME_LEN);
            
            cJSON *subj = cJSON_GetObjectItem(g, "subject");
            if (subj && cJSON_IsString(subj))
                strncpy(mg->subject, subj->valuestring, MAX_SUBJECT_LEN);
            
            cJSON *content = cJSON_GetObjectItem(g, "content");
            if (content && cJSON_IsString(content))
                strncpy(mg->body, content->valuestring, MAX_BODY_LEN);
            
            mg->enabled = true;
            
            cJSON *recips = cJSON_GetObjectItem(g, "recipients");
            if (recips && cJSON_IsArray(recips)) {
                int rc = cJSON_GetArraySize(recips);
                if (rc > MAX_RECIPIENTS) rc = MAX_RECIPIENTS;
                mg->recipient_count = rc;
                for (int j = 0; j < rc; j++) {
                    cJSON *r = cJSON_GetArrayItem(recips, j);
                    if (r && cJSON_IsString(r))
                        strncpy(mg->recipients[j], r->valuestring, MAX_EMAIL_LEN);
                }
            }
        }
    }
    
    cJSON_Delete(json);
    config_save_mail(&mail_cfg);
    LOG_CONFIG(LOG_LEVEL_INFO, "Mail groups updated");
    return web_send_success(req, "Mail groups saved");
}

/* ============================================
 * TELEGRAM CONFIG
 * ============================================ */

// POST /api/config/telegram (requires auth) - stub for future
static esp_err_t api_set_telegram_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[512];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    // TODO: Store telegram config when telegram support is added
    LOG_CONFIG(LOG_LEVEL_INFO, "Telegram config saved (placeholder)");
    return web_send_success(req, "Telegram config saved");
}

/* ============================================
 * WEBHOOK CONFIG
 * ============================================ */

// POST /api/config/webhook (requires auth) - stub for future
static esp_err_t api_set_webhook_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[512];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    // TODO: Store webhook config when webhook support is added
    LOG_CONFIG(LOG_LEVEL_INFO, "Webhook config saved (placeholder)");
    return web_send_success(req, "Webhook config saved");
}

/* ============================================
 * EARLY MAIL CONFIG
 * ============================================ */

// POST /api/config/early-mail (requires auth) - stub for future
static esp_err_t api_set_early_mail_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[1024];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    // TODO: Store early warning mail config
    LOG_CONFIG(LOG_LEVEL_INFO, "Early mail config saved (placeholder)");
    return web_send_success(req, "Early mail config saved");
}

/* ============================================
 * REBOOT
 * ============================================ */

// POST /api/reboot (requires auth)
static esp_err_t api_reboot_handler(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    LOG_CONFIG(LOG_LEVEL_WARN, "Reboot requested via API");
    esp_err_t ret = web_send_success(req, "Rebooting in 2 seconds");
    
    // Delay reboot to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    
    return ret; // Never reached
}

/* ============================================
 * FACTORY RESET
 * ============================================ */

// POST /api/factory-reset (requires auth)
static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    LOG_CONFIG(LOG_LEVEL_WARN, "Factory reset requested via API");
    
    esp_err_t reset_ret = config_factory_reset();
    if (reset_ret != ESP_OK) {
        return web_send_error(req, 500, "Factory reset failed");
    }
    
    esp_err_t ret = web_send_success(req, "Factory reset done, rebooting...");
    
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    
    return ret;
}

/* ============================================
 * SECURITY CONFIG (GET/POST)
 * ============================================ */

// GET /api/config/security (requires auth)
static esp_err_t api_get_security_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    auth_config_t auth;
    config_load_auth(&auth);
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "loginProtection", auth.max_login_attempts > 0);
    cJSON_AddNumberToObject(json, "lockoutTime", auth.lockout_duration_min);
    cJSON_AddBoolToObject(json, "resetApiEnabled", false); // TODO: persist this
    cJSON_AddStringToObject(json, "apiKey", ""); // TODO: persist API key
    cJSON_AddNumberToObject(json, "sessionTimeout", auth.session_timeout_min);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

// POST /api/config/security (requires auth)
static esp_err_t api_set_security_config(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char body[512];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    cJSON *json = cJSON_Parse(body);
    if (!json) return web_send_error(req, 400, "Invalid JSON");
    
    auth_config_t auth;
    config_load_auth(&auth);
    
    cJSON *loginProt = cJSON_GetObjectItem(json, "loginProtection");
    cJSON *lockout = cJSON_GetObjectItem(json, "lockoutTime");
    
    if (cJSON_IsBool(loginProt)) {
        auth.max_login_attempts = cJSON_IsTrue(loginProt) ? 5 : 0;
    }
    if (cJSON_IsNumber(lockout)) {
        auth.lockout_duration_min = lockout->valueint;
    }
    
    cJSON_Delete(json);
    
    esp_err_t save_ret = config_save_auth(&auth);
    if (save_ret != ESP_OK) {
        return web_send_error(req, 500, "Failed to save security config");
    }
    
    LOG_CONFIG(LOG_LEVEL_INFO, "Security config updated");
    return web_send_success(req, "Security config saved");
}

// POST /api/config/security/api-key (requires auth) - generate new API key
static esp_err_t api_refresh_api_key(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    // Generate random API key
    uint8_t rand_bytes[16];
    esp_fill_random(rand_bytes, sizeof(rand_bytes));
    char api_key[33];
    for (int i = 0; i < 16; i++) {
        sprintf(&api_key[i * 2], "%02x", rand_bytes[i]);
    }
    api_key[32] = '\0';
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "apiKey", api_key);
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    
    LOG_CONFIG(LOG_LEVEL_INFO, "API key regenerated");
    return ret;
}

/* ============================================
 * CONFIG EXPORT
 * ============================================ */

// POST /api/config/export (requires auth) - returns all configs as JSON blob
static esp_err_t api_config_export(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    // Verify auth password from body
    char body[256];
    int len = web_get_body(req, body, sizeof(body));
    if (len <= 0) return web_send_error(req, 400, "No body");
    
    cJSON *req_json = cJSON_Parse(body);
    if (!req_json) return web_send_error(req, 400, "Invalid JSON");
    
    cJSON *authPw = cJSON_GetObjectItem(req_json, "authPassword");
    if (!authPw || !cJSON_IsString(authPw)) {
        cJSON_Delete(req_json);
        return web_send_error(req, 400, "Missing authPassword");
    }
    
    if (!config_verify_password(authPw->valuestring)) {
        cJSON_Delete(req_json);
        return web_send_error(req, 403, "Wrong password");
    }
    cJSON_Delete(req_json);
    
    // Collect all configs
    cJSON *export_json = cJSON_CreateObject();
    cJSON_AddStringToObject(export_json, "type", "lebensspur-backup");
    cJSON_AddStringToObject(export_json, "version", "1.0");
    cJSON_AddStringToObject(export_json, "firmware", FIRMWARE_VERSION);
    
    // Timer config
    timer_config_t timer;
    if (config_load_timer(&timer) == ESP_OK) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddBoolToObject(t, "enabled", timer.enabled);
        cJSON_AddNumberToObject(t, "interval_minutes", timer.interval_minutes);
        cJSON_AddNumberToObject(t, "warning_minutes", timer.warning_minutes);
        cJSON_AddNumberToObject(t, "alarm_count", timer.alarm_count);
        cJSON_AddStringToObject(t, "check_start", timer.check_start);
        cJSON_AddStringToObject(t, "check_end", timer.check_end);
        cJSON_AddBoolToObject(t, "relay_trigger", timer.relay_trigger);
        cJSON_AddItemToObject(export_json, "timer", t);
    }
    
    // WiFi config
    app_wifi_config_t wifi;
    if (config_load_wifi(&wifi) == ESP_OK) {
        cJSON *w = cJSON_CreateObject();
        cJSON_AddStringToObject(w, "ssid", wifi.ssid);
        cJSON_AddStringToObject(w, "password", wifi.password);
        cJSON_AddItemToObject(export_json, "wifi", w);
    }
    
    // WiFi backup
    app_wifi_config_t wifi2;
    if (config_load_wifi_backup(&wifi2) == ESP_OK && wifi2.configured) {
        cJSON *w2 = cJSON_CreateObject();
        cJSON_AddStringToObject(w2, "ssid", wifi2.ssid);
        cJSON_AddStringToObject(w2, "password", wifi2.password);
        cJSON_AddItemToObject(export_json, "wifi_backup", w2);
    }
    
    // Mail/SMTP config
    mail_config_t mail;
    if (config_load_mail(&mail) == ESP_OK) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "smtp_server", mail.smtp_server);
        cJSON_AddNumberToObject(m, "smtp_port", mail.smtp_port);
        cJSON_AddStringToObject(m, "smtp_username", mail.smtp_username);
        cJSON_AddStringToObject(m, "smtp_password", mail.smtp_password);
        cJSON_AddStringToObject(m, "sender_name", mail.sender_name);
        cJSON_AddBoolToObject(m, "use_tls", mail.use_tls);
        
        cJSON *groups = cJSON_CreateArray();
        for (int i = 0; i < mail.group_count; i++) {
            cJSON *g = cJSON_CreateObject();
            cJSON_AddStringToObject(g, "name", mail.groups[i].name);
            cJSON_AddBoolToObject(g, "enabled", mail.groups[i].enabled);
            cJSON_AddStringToObject(g, "subject", mail.groups[i].subject);
            cJSON_AddStringToObject(g, "body", mail.groups[i].body);
            cJSON *recips = cJSON_CreateArray();
            for (int j = 0; j < mail.groups[i].recipient_count; j++) {
                cJSON_AddItemToArray(recips, cJSON_CreateString(mail.groups[i].recipients[j]));
            }
            cJSON_AddItemToObject(g, "recipients", recips);
            cJSON_AddItemToArray(groups, g);
        }
        cJSON_AddItemToObject(m, "groups", groups);
        cJSON_AddItemToObject(export_json, "mail", m);
    }
    
    // Relay config
    relay_config_t relay;
    if (config_load_relay(&relay) == ESP_OK) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "inverted", relay.inverted);
        cJSON_AddBoolToObject(r, "pulse_mode", relay.pulse_mode);
        cJSON_AddNumberToObject(r, "pulse_duration_ms", relay.pulse_duration_ms);
        cJSON_AddNumberToObject(r, "pulse_interval_ms", relay.pulse_interval_ms);
        cJSON_AddNumberToObject(r, "pulse_count", relay.pulse_count);
        cJSON_AddItemToObject(export_json, "relay", r);
    }
    
    char *json_str = cJSON_Print(export_json);
    cJSON_Delete(export_json);
    
    // Send as downloadable blob
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"lebensspur-backup.lsb\"");
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    
    LOG_CONFIG(LOG_LEVEL_INFO, "Config exported");
    return ret;
}

/* ============================================
 * CONFIG IMPORT
 * ============================================ */

// POST /api/config/import (requires auth) - restore configs from JSON
static esp_err_t api_config_import(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    char *body = malloc(4096);
    if (!body) return web_send_error(req, 500, "Out of memory");
    
    int len = web_get_body(req, body, 4096);
    if (len <= 0) {
        free(body);
        return web_send_error(req, 400, "No body");
    }
    
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return web_send_error(req, 400, "Invalid JSON");
    
    // The body contains {data: base64_encoded_json, password: "..."}
    cJSON *data_item = cJSON_GetObjectItem(json, "data");
    if (!data_item || !cJSON_IsString(data_item)) {
        cJSON_Delete(json);
        return web_send_error(req, 400, "Missing data field");
    }
    
    // The data is base64-encoded JSON (the actual backup content)
    // For now we decode the base64 and treat it as direct JSON
    // (Browser sends base64 from FileReader.readAsDataURL)
    const char *b64_data = data_item->valuestring;
    
    // Simple base64 decode - we just parse the raw JSON backup
    // Since handleImport sends base64 from readAsDataURL, we need to decode it
    // For simplicity, we'll use a smaller approach: just try parsing as JSON 
    // (the frontend can also send raw JSON)
    cJSON *backup = cJSON_Parse(b64_data);
    if (!backup) {
        // Try treating as a raw backup JSON string  
        cJSON_Delete(json);
        return web_send_error(req, 400, "Could not parse backup data");
    }
    cJSON_Delete(json);
    
    int restored = 0;
    
    // Restore timer config
    cJSON *timer_json = cJSON_GetObjectItem(backup, "timer");
    if (timer_json) {
        timer_config_t timer = TIMER_CONFIG_DEFAULT();
        cJSON *item;
        if ((item = cJSON_GetObjectItem(timer_json, "enabled"))) timer.enabled = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(timer_json, "interval_minutes"))) timer.interval_minutes = item->valueint;
        if ((item = cJSON_GetObjectItem(timer_json, "warning_minutes"))) timer.warning_minutes = item->valueint;
        if ((item = cJSON_GetObjectItem(timer_json, "alarm_count"))) timer.alarm_count = item->valueint;
        if ((item = cJSON_GetObjectItem(timer_json, "check_start")) && cJSON_IsString(item)) strncpy(timer.check_start, item->valuestring, 7);
        if ((item = cJSON_GetObjectItem(timer_json, "check_end")) && cJSON_IsString(item)) strncpy(timer.check_end, item->valuestring, 7);
        if ((item = cJSON_GetObjectItem(timer_json, "relay_trigger"))) timer.relay_trigger = cJSON_IsTrue(item);
        config_save_timer(&timer);
        restored++;
    }
    
    // Restore WiFi config
    cJSON *wifi_json = cJSON_GetObjectItem(backup, "wifi");
    if (wifi_json) {
        app_wifi_config_t wifi = WIFI_CONFIG_DEFAULT();
        cJSON *item;
        if ((item = cJSON_GetObjectItem(wifi_json, "ssid")) && cJSON_IsString(item)) strncpy(wifi.ssid, item->valuestring, MAX_SSID_LEN);
        if ((item = cJSON_GetObjectItem(wifi_json, "password")) && cJSON_IsString(item)) strncpy(wifi.password, item->valuestring, MAX_PASSWORD_LEN);
        wifi.configured = strlen(wifi.ssid) > 0;
        config_save_wifi(&wifi);
        restored++;
    }
    
    // Restore mail config
    cJSON *mail_json = cJSON_GetObjectItem(backup, "mail");
    if (mail_json) {
        mail_config_t mail = MAIL_CONFIG_DEFAULT();
        cJSON *item;
        if ((item = cJSON_GetObjectItem(mail_json, "smtp_server")) && cJSON_IsString(item)) strncpy(mail.smtp_server, item->valuestring, MAX_HOSTNAME_LEN);
        if ((item = cJSON_GetObjectItem(mail_json, "smtp_port"))) mail.smtp_port = item->valueint;
        if ((item = cJSON_GetObjectItem(mail_json, "smtp_username")) && cJSON_IsString(item)) strncpy(mail.smtp_username, item->valuestring, MAX_EMAIL_LEN);
        if ((item = cJSON_GetObjectItem(mail_json, "smtp_password")) && cJSON_IsString(item)) strncpy(mail.smtp_password, item->valuestring, MAX_PASSWORD_LEN);
        if ((item = cJSON_GetObjectItem(mail_json, "sender_name")) && cJSON_IsString(item)) strncpy(mail.sender_name, item->valuestring, MAX_GROUP_NAME_LEN);
        if ((item = cJSON_GetObjectItem(mail_json, "use_tls"))) mail.use_tls = cJSON_IsTrue(item);
        
        cJSON *groups = cJSON_GetObjectItem(mail_json, "groups");
        if (groups && cJSON_IsArray(groups)) {
            mail.group_count = 0;
            cJSON *g;
            cJSON_ArrayForEach(g, groups) {
                if (mail.group_count >= MAX_MAIL_GROUPS) break;
                mail_group_t *mg = &mail.groups[mail.group_count];
                *mg = (mail_group_t)MAIL_GROUP_DEFAULT();
                if ((item = cJSON_GetObjectItem(g, "name")) && cJSON_IsString(item)) strncpy(mg->name, item->valuestring, MAX_GROUP_NAME_LEN);
                if ((item = cJSON_GetObjectItem(g, "enabled"))) mg->enabled = cJSON_IsTrue(item);
                if ((item = cJSON_GetObjectItem(g, "subject")) && cJSON_IsString(item)) strncpy(mg->subject, item->valuestring, MAX_SUBJECT_LEN);
                if ((item = cJSON_GetObjectItem(g, "body")) && cJSON_IsString(item)) strncpy(mg->body, item->valuestring, MAX_BODY_LEN);
                cJSON *recips = cJSON_GetObjectItem(g, "recipients");
                if (recips && cJSON_IsArray(recips)) {
                    mg->recipient_count = 0;
                    cJSON *r;
                    cJSON_ArrayForEach(r, recips) {
                        if (mg->recipient_count >= MAX_RECIPIENTS) break;
                        if (cJSON_IsString(r)) {
                            strncpy(mg->recipients[mg->recipient_count], r->valuestring, MAX_EMAIL_LEN);
                            mg->recipient_count++;
                        }
                    }
                }
                mail.group_count++;
            }
        }
        config_save_mail(&mail);
        restored++;
    }
    
    // Restore relay config
    cJSON *relay_json = cJSON_GetObjectItem(backup, "relay");
    if (relay_json) {
        relay_config_t relay = RELAY_CONFIG_DEFAULT();
        cJSON *item;
        if ((item = cJSON_GetObjectItem(relay_json, "inverted"))) relay.inverted = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(relay_json, "pulse_mode"))) relay.pulse_mode = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItem(relay_json, "pulse_duration_ms"))) relay.pulse_duration_ms = item->valueint;
        if ((item = cJSON_GetObjectItem(relay_json, "pulse_interval_ms"))) relay.pulse_interval_ms = item->valueint;
        if ((item = cJSON_GetObjectItem(relay_json, "pulse_count"))) relay.pulse_count = item->valueint;
        config_save_relay(&relay);
        restored++;
    }
    
    cJSON_Delete(backup);
    
    LOG_CONFIG(LOG_LEVEL_INFO, "Config imported (%d sections)", restored);
    
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "restored", restored);
    cJSON_AddStringToObject(resp, "message", "Config imported successfully");
    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    esp_err_t ret = web_send_json(req, 200, resp_str);
    free(resp_str);
    return ret;
}

/* ============================================
 * OTA CHECK
 * ============================================ */

// GET /api/ota/check (requires auth)
static esp_err_t api_ota_check(httpd_req_t *req)
{
    if (!web_is_authenticated(req)) {
        return web_send_error(req, 401, "Unauthorized");
    }
    
    // Currently no OTA server configured - report up to date
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "updateAvailable", false);
    cJSON_AddStringToObject(json, "currentVersion", FIRMWARE_VERSION);
    cJSON_AddStringToObject(json, "message", "System is up to date");
    
    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

/* ============================================
 * GUI DOWNLOAD FROM GITHUB
 * ============================================ */

#define GUI_REPO_BASE  "https://raw.githubusercontent.com/smrtkrft/LebensSpur/main/GUI/"
#define GUI_DEST_DIR   "/ext/web"

typedef struct {
    const char *filename;
    const char *subdir;  // NULL = root, else "i18n" etc.
} gui_file_t;

static const gui_file_t s_gui_files[] = {
    { "index.html", NULL },
    { "style.css", NULL },
    { "manifest.json", NULL },
    { "sw.js", NULL },
    { "app.js", "js" },
    { "i18n.js", "js" },
    { "logo.png", "pic" },
    { "darklogo.png", "pic" },
    { "en.json", "i18n" },
    { "tr.json", "i18n" },
};
#define GUI_FILE_COUNT (sizeof(s_gui_files) / sizeof(s_gui_files[0]))

/* Download state (accessed from handler + task) */
static volatile int  s_dl_progress   = 0;   /* 0-100 */
static volatile bool s_dl_running    = false;
static volatile bool s_dl_done       = false;
static volatile bool s_dl_error      = false;
static char          s_dl_msg[64]    = "Idle";

static esp_err_t download_one_file(const char *filename, const char *subdir)
{
    char url[196];
    char path[80];
    if (subdir) {
        snprintf(url, sizeof(url), GUI_REPO_BASE "%s/%s", subdir, filename);
        // SPIFFS is flat - no mkdir needed, '/' is part of filename
        snprintf(path, sizeof(path), GUI_DEST_DIR "/%s/%s", subdir, filename);
    } else {
        snprintf(url, sizeof(url), GUI_REPO_BASE "%s", filename);
        snprintf(path, sizeof(path), GUI_DEST_DIR "/%s", filename);
    }

    ESP_LOGI(TAG, "DL %s", filename);

    esp_http_client_config_t cfg = {
        .url             = url,
        .timeout_ms      = 30000,
        .buffer_size     = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_len = esp_http_client_fetch_headers(client);
    (void)content_len;   /* may be -1 for chunked */
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "%s HTTP %d", filename, status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Read into temp buffer and write to SPIFFS */
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create %s", path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    int rd;
    while ((rd = esp_http_client_read(client, buf, 4096)) > 0) {
        fwrite(buf, 1, rd, f);
        total += rd;
    }

    free(buf);
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Saved %s (%d bytes)", filename, total);
    return ESP_OK;
}

static void gui_download_task(void *arg)
{
    s_dl_running = true;
    s_dl_done    = false;
    s_dl_error   = false;

    /* Ensure DNS is configured (fallback to Google DNS if empty) */
    const ip_addr_t *d0 = dns_getserver(0);
    if (ip_addr_isany(d0)) {
        ip_addr_t gdns;
        IP_ADDR4(&gdns, 8, 8, 8, 8);
        dns_setserver(0, &gdns);
        IP_ADDR4(&gdns, 8, 8, 4, 4);
        dns_setserver(1, &gdns);
    }

    /* Wait for network to stabilize */
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (int i = 0; i < GUI_FILE_COUNT; i++) {
        snprintf(s_dl_msg, sizeof(s_dl_msg), "Downloading GUI...");
        s_dl_progress = (i * 100) / GUI_FILE_COUNT;

        /* Retry up to 3 times per file */
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < 3; attempt++) {
            err = download_one_file(s_gui_files[i].filename, s_gui_files[i].subdir);
            if (err == ESP_OK) break;
            ESP_LOGW(TAG, "Retry %d for %s", attempt + 1, s_gui_files[i].filename);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        if (err != ESP_OK) {
            snprintf(s_dl_msg, sizeof(s_dl_msg), "Download failed");
            s_dl_error   = true;
            s_dl_running = false;
            vTaskDelete(NULL);
            return;
        }
    }

    s_dl_progress = 100;
    snprintf(s_dl_msg, sizeof(s_dl_msg), "Download complete");
    s_dl_done    = true;
    s_dl_running = false;
    vTaskDelete(NULL);
}

// POST /api/gui/download - Start GUI download from GitHub
static esp_err_t api_gui_download(httpd_req_t *req)
{
    if (s_dl_running) {
        return web_send_error(req, 409, "Download already in progress");
    }

    ESP_LOGI(TAG, "Starting GUI download from GitHub");

    s_dl_progress = 0;
    s_dl_done     = false;
    s_dl_error    = false;
    snprintf(s_dl_msg, sizeof(s_dl_msg), "Downloading GUI...");

    BaseType_t ok = xTaskCreate(gui_download_task, "gui_dl", 16384, NULL, 5, NULL);
    if (ok != pdPASS) {
        return web_send_error(req, 500, "Cannot start download task");
    }

    return web_send_success(req, "Download started");
}

// GET /api/gui/download/status - GUI download progress
static esp_err_t api_gui_download_status(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();

    if (s_dl_error) {
        cJSON_AddStringToObject(json, "status", "error");
        cJSON_AddStringToObject(json, "error", s_dl_msg);
    } else if (s_dl_done) {
        cJSON_AddStringToObject(json, "status", "done");
        cJSON_AddNumberToObject(json, "downloaded", (int)GUI_FILE_COUNT);
    } else if (s_dl_running) {
        cJSON_AddStringToObject(json, "status", "downloading");
    } else {
        cJSON_AddStringToObject(json, "status", "idle");
    }

    cJSON_AddNumberToObject(json, "progress", s_dl_progress);
    cJSON_AddStringToObject(json, "message", s_dl_msg);

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    esp_err_t ret = web_send_json(req, 200, json_str);
    free(json_str);
    return ret;
}

/* ============================================
 * STATIC FILE HANDLER
 * ============================================ */

static esp_err_t static_file_handler(httpd_req_t *req)
{
    char filepath[FILE_MGR_MAX_PATH_LEN];
    char uri_copy[128];
    const char *uri = req->uri;
    bool is_root = (strcmp(uri, "/") == 0);
    
    // Default to index.html
    if (is_root) {
        uri = "/index.html";
    }
    
    // Truncate URI to prevent overflow
    strncpy(uri_copy, uri, sizeof(uri_copy) - 1);
    uri_copy[sizeof(uri_copy) - 1] = '\0';
    
    // Build filepath
    snprintf(filepath, sizeof(filepath), "%s%s", WEB_STATIC_DIR, uri_copy);
    
    // Security: prevent path traversal
    if (strstr(filepath, "..") != NULL) {
        return web_send_error(req, 403, "Forbidden");
    }
    
    // Check if file exists, if root and index.html missing, redirect to setup
    if (is_root && !file_manager_exists(filepath)) {
        ESP_LOGI(TAG, "index.html not found, redirecting to setup");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup.html");
        return httpd_resp_send(req, NULL, 0);
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
    
    // Timer status
    httpd_uri_t timer_status_route = {
        .uri = "/api/timer/status",
        .method = HTTP_GET,
        .handler = api_timer_status
    };
    httpd_register_uri_handler(server, &timer_status_route);
    
    // Timer enable/disable
    httpd_uri_t timer_enable = {
        .uri = "/api/timer/enable",
        .method = HTTP_POST,
        .handler = api_timer_enable
    };
    httpd_register_uri_handler(server, &timer_enable);
    
    httpd_uri_t timer_disable = {
        .uri = "/api/timer/disable",
        .method = HTTP_POST,
        .handler = api_timer_disable
    };
    httpd_register_uri_handler(server, &timer_disable);
    
    // Timer acknowledge
    httpd_uri_t timer_ack = {
        .uri = "/api/timer/acknowledge",
        .method = HTTP_POST,
        .handler = api_timer_acknowledge
    };
    httpd_register_uri_handler(server, &timer_ack);
    
    // Vacation mode
    httpd_uri_t vacation = {
        .uri = "/api/timer/vacation",
        .method = HTTP_POST,
        .handler = api_timer_vacation
    };
    httpd_register_uri_handler(server, &vacation);
    
    // Relay test
    httpd_uri_t relay_test = {
        .uri = "/api/relay/test",
        .method = HTTP_POST,
        .handler = api_relay_test
    };
    httpd_register_uri_handler(server, &relay_test);
    
    // Relay config
    httpd_uri_t relay_get = {
        .uri = "/api/config/relay",
        .method = HTTP_GET,
        .handler = api_get_relay_config
    };
    httpd_register_uri_handler(server, &relay_get);
    
    httpd_uri_t relay_set = {
        .uri = "/api/config/relay",
        .method = HTTP_POST,
        .handler = api_set_relay_config
    };
    httpd_register_uri_handler(server, &relay_set);
    
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
    
    // Setup APIs (no auth required)
    httpd_uri_t setup_wifi_scan = {
        .uri = "/api/setup/wifi/scan",
        .method = HTTP_GET,
        .handler = api_setup_wifi_scan
    };
    httpd_register_uri_handler(server, &setup_wifi_scan);
    
    httpd_uri_t setup_wifi_connect = {
        .uri = "/api/setup/wifi/connect",
        .method = HTTP_POST,
        .handler = api_setup_wifi_connect
    };
    httpd_register_uri_handler(server, &setup_wifi_connect);
    
    httpd_uri_t setup_password = {
        .uri = "/api/setup/password",
        .method = HTTP_POST,
        .handler = api_setup_password
    };
    httpd_register_uri_handler(server, &setup_password);
    
    httpd_uri_t setup_complete = {
        .uri = "/api/setup/complete",
        .method = HTTP_POST,
        .handler = api_setup_complete
    };
    httpd_register_uri_handler(server, &setup_complete);
    
    // WiFi status endpoint (for setup wizard)
    httpd_uri_t wifi_status = {
        .uri = "/api/wifi/status",
        .method = HTTP_GET,
        .handler = api_wifi_status
    };
    httpd_register_uri_handler(server, &wifi_status);
    
    // Device info endpoint
    httpd_uri_t device_info = {
        .uri = "/api/device/info",
        .method = HTTP_GET,
        .handler = api_device_info
    };
    httpd_register_uri_handler(server, &device_info);
    
    // GUI download endpoints (for setup wizard)
    httpd_uri_t gui_download = {
        .uri = "/api/gui/download",
        .method = HTTP_POST,
        .handler = api_gui_download
    };
    httpd_register_uri_handler(server, &gui_download);
    
    httpd_uri_t gui_download_status = {
        .uri = "/api/gui/download/status",
        .method = HTTP_GET,
        .handler = api_gui_download_status
    };
    httpd_register_uri_handler(server, &gui_download_status);
    
    // WiFi config endpoints (requires auth)
    httpd_uri_t wifi_config_get = {
        .uri = "/api/config/wifi",
        .method = HTTP_GET,
        .handler = api_get_wifi_config
    };
    httpd_register_uri_handler(server, &wifi_config_get);
    
    httpd_uri_t wifi_config_set = {
        .uri = "/api/config/wifi",
        .method = HTTP_POST,
        .handler = api_set_wifi_config
    };
    httpd_register_uri_handler(server, &wifi_config_set);
    
    // SMTP config endpoints (requires auth)
    httpd_uri_t smtp_config_get = {
        .uri = "/api/config/smtp",
        .method = HTTP_GET,
        .handler = api_get_smtp_config
    };
    httpd_register_uri_handler(server, &smtp_config_get);
    
    httpd_uri_t smtp_config_set = {
        .uri = "/api/config/smtp",
        .method = HTTP_POST,
        .handler = api_set_smtp_config
    };
    httpd_register_uri_handler(server, &smtp_config_set);
    
    // AP mode config endpoint (requires auth)
    httpd_uri_t ap_config_set = {
        .uri = "/api/config/ap",
        .method = HTTP_POST,
        .handler = api_set_ap_config
    };
    httpd_register_uri_handler(server, &ap_config_set);
    
    // Password change endpoint
    httpd_uri_t password_change = {
        .uri = "/api/config/password",
        .method = HTTP_POST,
        .handler = api_change_password
    };
    httpd_register_uri_handler(server, &password_change);
    
    // Mail groups endpoints
    httpd_uri_t mail_groups_get = {
        .uri = "/api/config/mail-groups",
        .method = HTTP_GET,
        .handler = api_get_mail_groups
    };
    httpd_register_uri_handler(server, &mail_groups_get);
    
    httpd_uri_t mail_groups_set = {
        .uri = "/api/config/mail-groups",
        .method = HTTP_POST,
        .handler = api_set_mail_groups
    };
    httpd_register_uri_handler(server, &mail_groups_set);
    
    // Telegram config endpoint
    httpd_uri_t telegram_config = {
        .uri = "/api/config/telegram",
        .method = HTTP_POST,
        .handler = api_set_telegram_config
    };
    httpd_register_uri_handler(server, &telegram_config);
    
    // Webhook config endpoint
    httpd_uri_t webhook_config = {
        .uri = "/api/config/webhook",
        .method = HTTP_POST,
        .handler = api_set_webhook_config
    };
    httpd_register_uri_handler(server, &webhook_config);
    
    // Early mail config endpoint
    httpd_uri_t early_mail_config = {
        .uri = "/api/config/early-mail",
        .method = HTTP_POST,
        .handler = api_set_early_mail_config
    };
    httpd_register_uri_handler(server, &early_mail_config);
    
    // OTA check endpoint
    httpd_uri_t ota_check = {
        .uri = "/api/ota/check",
        .method = HTTP_GET,
        .handler = api_ota_check
    };
    httpd_register_uri_handler(server, &ota_check);
    
    // Reboot endpoint
    httpd_uri_t reboot = {
        .uri = "/api/reboot",
        .method = HTTP_POST,
        .handler = api_reboot_handler
    };
    httpd_register_uri_handler(server, &reboot);
    
    // Factory reset endpoint
    httpd_uri_t factory_reset = {
        .uri = "/api/factory-reset",
        .method = HTTP_POST,
        .handler = api_factory_reset_handler
    };
    httpd_register_uri_handler(server, &factory_reset);
    
    // Security config GET
    httpd_uri_t security_get = {
        .uri = "/api/config/security",
        .method = HTTP_GET,
        .handler = api_get_security_config
    };
    httpd_register_uri_handler(server, &security_get);
    
    // Security config POST
    httpd_uri_t security_set = {
        .uri = "/api/config/security",
        .method = HTTP_POST,
        .handler = api_set_security_config
    };
    httpd_register_uri_handler(server, &security_set);
    
    // API key refresh
    httpd_uri_t api_key_refresh = {
        .uri = "/api/config/security/api-key",
        .method = HTTP_POST,
        .handler = api_refresh_api_key
    };
    httpd_register_uri_handler(server, &api_key_refresh);
    
    // Config export
    httpd_uri_t config_export = {
        .uri = "/api/config/export",
        .method = HTTP_POST,
        .handler = api_config_export
    };
    httpd_register_uri_handler(server, &config_export);
    
    // Config import
    httpd_uri_t config_import = {
        .uri = "/api/config/import",
        .method = HTTP_POST,
        .handler = api_config_import
    };
    httpd_register_uri_handler(server, &config_import);
    
    ESP_LOGI(TAG, "API routes registered");
}

void web_register_static_routes(httpd_handle_t server)
{
    // Setup page - embedded in firmware (always available)
    httpd_uri_t setup_uri = {
        .uri = "/setup.html",
        .method = HTTP_GET,
        .handler = setup_html_handler
    };
    httpd_register_uri_handler(server, &setup_uri);
    
    // Also serve setup on root when no index.html exists
    httpd_uri_t setup_root = {
        .uri = "/setup",
        .method = HTTP_GET,
        .handler = setup_html_handler
    };
    httpd_register_uri_handler(server, &setup_root);
    
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
