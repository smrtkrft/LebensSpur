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
#include <sys/socket.h>

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

// GET /api/device/info - Device information
static esp_err_t api_device_info(httpd_req_t *req)
{
    wifi_status_t wifi_status;
    wifi_manager_get_status(&wifi_status);
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "device_id", device_id_get());
    cJSON_AddStringToObject(json, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(json, "sta_ip", wifi_status.sta_ip);
    cJSON_AddStringToObject(json, "ap_ip", wifi_status.ap_ip);
    cJSON_AddBoolToObject(json, "sta_connected", wifi_status.sta_connected);
    cJSON_AddStringToObject(json, "hostname", wifi_manager_get_hostname());
    
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
} gui_file_t;

static const gui_file_t s_gui_files[] = {
    { "index.html" },
    { "app.js" },
    { "style.css" },
    { "i18n.js" },
    { "manifest.json" },
    { "sw.js" },
    { "logo.png" },
    { "darklogo.png" },
};
#define GUI_FILE_COUNT (sizeof(s_gui_files) / sizeof(s_gui_files[0]))

/* Download state (accessed from handler + task) */
static volatile int  s_dl_progress   = 0;   /* 0-100 */
static volatile bool s_dl_running    = false;
static volatile bool s_dl_done       = false;
static volatile bool s_dl_error      = false;
static char          s_dl_msg[64]    = "Idle";

static esp_err_t download_one_file(const char *filename)
{
    char url[196];
    char path[64];
    snprintf(url, sizeof(url), GUI_REPO_BASE "%s", filename);
    snprintf(path, sizeof(path), GUI_DEST_DIR "/%s", filename);

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
            err = download_one_file(s_gui_files[i].filename);
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
        cJSON_AddStringToObject(json, "state", "error");
        cJSON_AddStringToObject(json, "error", s_dl_msg);
    } else if (s_dl_done) {
        cJSON_AddStringToObject(json, "state", "complete");
    } else {
        cJSON_AddStringToObject(json, "state", "downloading");
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
