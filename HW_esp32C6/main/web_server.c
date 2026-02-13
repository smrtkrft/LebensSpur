/**
 * Web Server - HTTP API ve Statik Dosya Servisi
 *
 * Bearer token + Cookie fallback ile kimlik dogrulama.
 * REST API: timer, mail, wifi, relay, ota, device, setup.
 * LittleFS'ten statik dosya servisi.
 */

#include "web_server.h"
#include "file_manager.h"
#include "web_assets.h"
#include "session_auth.h"
#include "config_manager.h"
#include "timer_scheduler.h"
#include "mail_sender.h"
#include "wifi_manager.h"
#include "relay_manager.h"
#include "gui_downloader.h"
#include "ota_manager.h"
#include "device_id.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_manager.h"
#include "time_manager.h"
#include "ext_flash.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include <string.h>
#include <cJSON.h>

static const char *TAG = "WEB";

static httpd_handle_t s_server = NULL;
static uint32_t s_request_count = 0;

// ============================================================================
// Auth yardimcilari
// ============================================================================

// Bearer token + Cookie fallback ile session dogrulama
static bool check_auth(httpd_req_t *req)
{
    char token[SESSION_TOKEN_LEN + 1] = {0};

    // 1. Authorization: Bearer <token>
    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) == ESP_OK) {
        if (session_extract_bearer_token(auth_hdr, token)) {
            return session_validate(token);
        }
    }

    // 2. Cookie fallback
    char cookie_hdr[512] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) == ESP_OK) {
        if (session_extract_cookie_token(cookie_hdr, token)) {
            return session_validate(token);
        }
    }

    return false;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    if (strncmp(req->uri, "/api/", 5) == 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login.html");
    return httpd_resp_send(req, NULL, 0);
}

// POST body oku (max_len'e kadar)
static int read_body(httpd_req_t *req, char *buf, size_t max_len)
{
    int len = req->content_len;
    if (len <= 0 || (size_t)len >= max_len) return -1;
    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) return -1;
    buf[received] = '\0';
    return received;
}

// MIME type
static const char *get_mime(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".html") == 0) return "text/html";
    if (strcasecmp(ext, ".css") == 0) return "text/css";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain";
    return "application/octet-stream";
}

// ============================================================================
// Sayfa handler'lari
// ============================================================================

// Ana sayfa
static esp_err_t h_index(httpd_req_t *req)
{
    s_request_count++;

    // Setup tamamlanmamissa yonlendir
    if (!config_is_setup_completed()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup.html");
        return httpd_resp_send(req, NULL, 0);
    }

    // Auth kontrolu
    if (!check_auth(req)) {
        return send_unauthorized(req);
    }

    // Harici flash'ta GUI varsa oradan servis et
    if (gui_downloader_files_exist()) {
        char fp[64];
        snprintf(fp, sizeof(fp), "%s/index.html", FILE_MGR_WEB_PATH);
        if (file_manager_exists(fp)) {
            return web_server_send_file(req, fp);
        }
    }

    // Fallback: gomulu HTML
    const char *html = web_assets_get_index_html();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// Login sayfasi (auth gerektirmez)
static esp_err_t h_login_page(httpd_req_t *req)
{
    s_request_count++;
    const char *html = web_assets_get_login_html();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// Setup sayfasi (auth gerektirmez)
static esp_err_t h_setup_page(httpd_req_t *req)
{
    s_request_count++;
    const char *html = web_assets_get_setup_html();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// ============================================================================
// Login / Logout API
// ============================================================================

static esp_err_t h_api_login(httpd_req_t *req)
{
    s_request_count++;
    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *pw = cJSON_GetObjectItem(json, "password");
    const char *password = cJSON_IsString(pw) ? pw->valuestring : "";

    bool ok = session_check_password(password);
    cJSON_Delete(json);

    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return web_server_send_json(req, "{\"success\":false,\"error\":\"Wrong password\"}");
    }

    // Session olustur
    char token[SESSION_TOKEN_LEN + 1];
    if (session_create(token) != ESP_OK) {
        return web_server_send_error(req, 500, "Session error");
    }

    // Cookie de ayarla (browser fallback)
    char cookie[128];
    session_format_cookie(token, cookie, sizeof(cookie));
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    // Token'i JSON response'ta dondur
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"success\":true,\"token\":\"%s\"}", token);
    return web_server_send_json(req, resp);
}

static esp_err_t h_api_logout(httpd_req_t *req)
{
    s_request_count++;

    // Token'i bul ve session'i sil
    char token[SESSION_TOKEN_LEN + 1] = {0};
    char auth_hdr[128] = {0};
    char cookie_hdr[512] = {0};

    httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr));
    httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr));

    if (session_extract_token(auth_hdr[0] ? auth_hdr : NULL,
                              cookie_hdr[0] ? cookie_hdr : NULL, token)) {
        session_destroy(token);
    }

    // Cookie sil
    char logout_cookie[128];
    session_format_logout_cookie(logout_cookie, sizeof(logout_cookie));
    httpd_resp_set_hdr(req, "Set-Cookie", logout_cookie);

    return web_server_send_json(req, "{\"success\":true}");
}

// ============================================================================
// Timer API
// ============================================================================

static esp_err_t h_api_timer_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    timer_status_t st;
    if (timer_get_status(&st) != ESP_OK) {
        return web_server_send_error(req, 500, "Timer error");
    }

    // State enum → string (app.js expects string states)
    const char *state_names[] = {"DISABLED", "RUNNING", "WARNING", "TRIGGERED", "PAUSED"};
    const char *state_str = (st.state <= TIMER_STATE_PAUSED) ? state_names[st.state] : "DISABLED";

    // Load config for intervalMinutes
    timer_config_t cfg;
    if (config_load_timer(&cfg) != ESP_OK) {
        timer_config_t def = TIMER_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    char json[640];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\","
        "\"timeRemainingMs\":%llu,"
        "\"intervalMinutes\":%lu,"
        "\"warningsSent\":%lu,"
        "\"resetCount\":%lu,"
        "\"triggerCount\":%lu,"
        "\"enabled\":%s,"
        "\"vacationEnabled\":false,"
        "\"vacationDays\":0}",
        state_str,
        (unsigned long long)st.remaining_seconds * 1000ULL,
        (unsigned long)(cfg.interval_hours * 60),
        st.warning_count,
        st.reset_count,
        st.trigger_count,
        cfg.enabled ? "true" : "false");

    return web_server_send_json(req, json);
}

static esp_err_t h_api_timer_reset(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    if (timer_reset() == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Timer reset failed");
}

static esp_err_t h_api_config_timer_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    timer_config_t cfg;
    if (config_load_timer(&cfg) != ESP_OK) {
        timer_config_t def = TIMER_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    // app.js expects intervalMinutes, alarmCount, vacationEnabled, vacationDays
    uint32_t interval_min = cfg.interval_hours * 60;
    uint32_t alarm_count = 0;
    if (cfg.warning_minutes > 0 && interval_min > 0) {
        alarm_count = cfg.warning_minutes / (interval_min > 60 ? 60 : 1);
        if (alarm_count == 0) alarm_count = 1;
    }

    char json[256];
    snprintf(json, sizeof(json),
        "{\"intervalMinutes\":%lu,"
        "\"alarmCount\":%lu,"
        "\"vacationEnabled\":false,"
        "\"vacationDays\":7}",
        (unsigned long)interval_min,
        (unsigned long)alarm_count);

    return web_server_send_json(req, json);
}

static esp_err_t h_api_config_timer_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    timer_config_t cfg;
    config_load_timer(&cfg);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "enabled"))) cfg.enabled = cJSON_IsTrue(it);

    // app.js sends intervalMinutes → convert to interval_hours
    if ((it = cJSON_GetObjectItem(json, "intervalMinutes"))) {
        cfg.interval_hours = (uint32_t)(it->valuedouble / 60.0);
        if (cfg.interval_hours == 0) cfg.interval_hours = 1;
    }
    if ((it = cJSON_GetObjectItem(json, "interval_hours"))) cfg.interval_hours = it->valueint;

    // app.js sends warningMinutes
    if ((it = cJSON_GetObjectItem(json, "warningMinutes"))) cfg.warning_minutes = it->valueint;
    if ((it = cJSON_GetObjectItem(json, "warning_minutes"))) cfg.warning_minutes = it->valueint;

    if ((it = cJSON_GetObjectItem(json, "check_start")) && cJSON_IsString(it))
        strncpy(cfg.check_start, it->valuestring, sizeof(cfg.check_start) - 1);
    if ((it = cJSON_GetObjectItem(json, "check_end")) && cJSON_IsString(it))
        strncpy(cfg.check_end, it->valuestring, sizeof(cfg.check_end) - 1);
    if ((it = cJSON_GetObjectItem(json, "relay_action")) && cJSON_IsString(it))
        strncpy(cfg.relay_action, it->valuestring, sizeof(cfg.relay_action) - 1);

    cJSON_Delete(json);

    if (config_save_timer(&cfg) == ESP_OK) {
        timer_set_enabled(cfg.enabled);
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// Mail API
// ============================================================================

static esp_err_t h_api_config_mail_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    mail_config_t cfg;
    if (config_load_mail(&cfg) != ESP_OK) {
        mail_config_t def = MAIL_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    char json[512];
    snprintf(json, sizeof(json),
        "{\"server\":\"%s\","
        "\"port\":%d,"
        "\"username\":\"%s\","
        "\"password\":\"%s\","
        "\"sender_name\":\"%s\"}",
        cfg.server, cfg.port, cfg.username,
        cfg.password[0] ? "********" : "",
        cfg.sender_name);

    return web_server_send_json(req, json);
}

static esp_err_t h_api_config_mail_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    mail_config_t cfg;
    config_load_mail(&cfg);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "server")) && cJSON_IsString(it))
        strncpy(cfg.server, it->valuestring, sizeof(cfg.server) - 1);
    if ((it = cJSON_GetObjectItem(json, "port")))
        cfg.port = it->valueint;
    if ((it = cJSON_GetObjectItem(json, "username")) && cJSON_IsString(it))
        strncpy(cfg.username, it->valuestring, sizeof(cfg.username) - 1);
    if ((it = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(it)) {
        if (strcmp(it->valuestring, "********") != 0 && strlen(it->valuestring) > 0)
            strncpy(cfg.password, it->valuestring, sizeof(cfg.password) - 1);
    }
    if ((it = cJSON_GetObjectItem(json, "sender_name")) && cJSON_IsString(it))
        strncpy(cfg.sender_name, it->valuestring, sizeof(cfg.sender_name) - 1);

    cJSON_Delete(json);

    if (config_save_mail(&cfg) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

static esp_err_t h_api_mail_test(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *to_item = cJSON_GetObjectItem(json, "to");
    const char *to = cJSON_IsString(to_item) ? to_item->valuestring : NULL;

    if (!to || strlen(to) == 0) {
        cJSON_Delete(json);
        return web_server_send_error(req, 400, "Missing 'to'");
    }

    esp_err_t ret = mail_send_test(to);
    cJSON_Delete(json);

    if (ret == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true,\"message\":\"Test mail queued\"}");
    }
    return web_server_send_error(req, 500, "Mail queue failed");
}

static esp_err_t h_api_mail_stats(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    mail_stats_t stats;
    mail_get_stats(&stats);

    char json[256];
    snprintf(json, sizeof(json),
        "{\"total_sent\":%lu,\"total_failed\":%lu,\"queue_count\":%lu,\"last_send_time\":%lu}",
        stats.total_sent, stats.total_failed, stats.queue_count, stats.last_send_time);

    return web_server_send_json(req, json);
}

// ============================================================================
// SMTP Config API (separate from mail - different field names for GUI)
// ============================================================================

static esp_err_t h_api_config_smtp_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    mail_config_t cfg;
    if (config_load_mail(&cfg) != ESP_OK) {
        mail_config_t def = MAIL_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    // app.js expects: smtpServer, smtpPort, smtpUsername, smtpPassword
    char json[512];
    snprintf(json, sizeof(json),
        "{\"smtpServer\":\"%s\","
        "\"smtpPort\":%d,"
        "\"smtpUsername\":\"%s\","
        "\"smtpPassword\":\"%s\"}",
        cfg.server, cfg.port, cfg.username,
        cfg.password[0] ? "********" : "");

    return web_server_send_json(req, json);
}

static esp_err_t h_api_config_smtp_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    mail_config_t cfg;
    config_load_mail(&cfg);

    cJSON *it;
    // app.js sends: smtpServer, smtpPort, smtpUsername, smtpPassword
    if ((it = cJSON_GetObjectItem(json, "smtpServer")) && cJSON_IsString(it))
        strncpy(cfg.server, it->valuestring, sizeof(cfg.server) - 1);
    if ((it = cJSON_GetObjectItem(json, "smtpPort")))
        cfg.port = it->valueint;
    if ((it = cJSON_GetObjectItem(json, "smtpUsername")) && cJSON_IsString(it))
        strncpy(cfg.username, it->valuestring, sizeof(cfg.username) - 1);
    if ((it = cJSON_GetObjectItem(json, "smtpPassword")) && cJSON_IsString(it)) {
        if (strcmp(it->valuestring, "********") != 0 && strlen(it->valuestring) > 0)
            strncpy(cfg.password, it->valuestring, sizeof(cfg.password) - 1);
    }

    cJSON_Delete(json);

    if (config_save_mail(&cfg) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// Device Info API
// ============================================================================

static esp_err_t h_api_device_info(httpd_req_t *req)
{
    // Setup modunda auth gerekmez
    if (config_is_setup_completed() && !check_auth(req)) {
        return send_unauthorized(req);
    }
    s_request_count++;

    cJSON *root = cJSON_CreateObject();
    if (!root) return web_server_send_error(req, 500, "No memory");

    const char *dev_id = device_id_get();

    // Identity
    cJSON_AddStringToObject(root, "device_id", dev_id);
    cJSON_AddStringToObject(root, "firmware", ota_manager_get_current_version());
    cJSON_AddStringToObject(root, "hostname", dev_id);

    // Chip info
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON_AddStringToObject(root, "chip_model", "ESP32-C6");
    cJSON_AddNumberToObject(root, "chip_cores", chip.cores);
    cJSON_AddNumberToObject(root, "cpu_freq_mhz", 160);

    // MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);

    // Heap
    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t heap_free = esp_get_free_heap_size();
    size_t heap_min = esp_get_minimum_free_heap_size();
    cJSON_AddNumberToObject(root, "heap_total", (double)heap_total);
    cJSON_AddNumberToObject(root, "heap_free", (double)heap_free);
    cJSON_AddNumberToObject(root, "heap_min_free", (double)heap_min);

    // Internal flash (from running partition)
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint32_t int_flash = 0;
    uint32_t app_size = 0;
    if (running) {
        int_flash = 4 * 1024 * 1024; // 4MB internal flash
        app_size = running->size;
    }
    cJSON_AddNumberToObject(root, "int_flash_total", (double)int_flash);
    cJSON_AddNumberToObject(root, "app_size", (double)app_size);
    cJSON_AddNumberToObject(root, "ota_size", (double)app_size);
    cJSON_AddNumberToObject(root, "nvs_size", 24576.0);

    // External flash
    uint32_t ext_total = ext_flash_get_size();
    cJSON_AddNumberToObject(root, "ext_flash_total", (double)ext_total);

    // LittleFS filesystem info
    uint32_t fs_total = 0, fs_used = 0;
    file_manager_get_info(&fs_total, &fs_used);
    cJSON_AddNumberToObject(root, "fs_cfg_total", (double)fs_total);
    cJSON_AddNumberToObject(root, "fs_cfg_used", (double)fs_used);
    cJSON_AddNumberToObject(root, "fs_gui_total", 0);
    cJSON_AddNumberToObject(root, "fs_gui_used", 0);
    cJSON_AddNumberToObject(root, "fs_data_total", 0);
    cJSON_AddNumberToObject(root, "fs_data_used", 0);

    // WiFi STA
    bool sta_connected = wifi_manager_is_connected();
    cJSON_AddBoolToObject(root, "sta_connected", sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", wifi_manager_get_ip());

    // STA SSID and RSSI via esp_wifi API
    wifi_ap_record_t ap_info;
    if (sta_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddStringToObject(root, "sta_ssid", (const char *)ap_info.ssid);
        cJSON_AddNumberToObject(root, "sta_rssi", ap_info.rssi);
    } else {
        cJSON_AddStringToObject(root, "sta_ssid", "");
        cJSON_AddNumberToObject(root, "sta_rssi", 0);
    }

    // WiFi AP
    ls_wifi_config_t wcfg;
    bool ap_active = true;
    if (config_load_wifi(&wcfg) == ESP_OK) {
        ap_active = wcfg.ap_mode_enabled;
    }
    cJSON_AddBoolToObject(root, "ap_active", ap_active);
    cJSON_AddStringToObject(root, "ap_ip", wifi_manager_get_ap_ip());
    cJSON_AddStringToObject(root, "ap_ssid", wifi_manager_get_ap_ssid());

    // Uptime (seconds)
    cJSON_AddNumberToObject(root, "uptime_s", (double)time_manager_get_uptime_sec());

    // Reset reason
    cJSON_AddNumberToObject(root, "reset_reason", (double)esp_reset_reason());

    // NTP
    cJSON_AddBoolToObject(root, "ntp_synced", time_manager_is_synced());
    char time_str[TIME_STR_MAX_LEN];
    time_manager_get_time_str(time_str, sizeof(time_str), NULL);
    cJSON_AddStringToObject(root, "time", time_str);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

// ============================================================================
// Relay API
// ============================================================================

static esp_err_t h_api_relay_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    relay_status_t st;
    relay_get_status(&st);

    const char *snames[] = {"idle", "delay", "active", "pulsing"};

    char json[256];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"gpio_level\":%d,\"energy_output\":%s,"
        "\"remaining_delay\":%lu,\"remaining_duration\":%lu,"
        "\"pulse_count\":%lu,\"trigger_count\":%lu}",
        snames[st.state], st.gpio_level,
        st.energy_output ? "true" : "false",
        st.remaining_delay, st.remaining_duration,
        st.pulse_count, st.trigger_count);

    return web_server_send_json(req, json);
}

static esp_err_t h_api_relay_control(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[128] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *act = cJSON_GetObjectItem(json, "action");
    const char *action = cJSON_IsString(act) ? act->valuestring : "";

    esp_err_t ret = ESP_FAIL;
    if (strcmp(action, "on") == 0) ret = relay_on();
    else if (strcmp(action, "off") == 0) ret = relay_off();
    else if (strcmp(action, "toggle") == 0) ret = relay_toggle();
    else if (strcmp(action, "trigger") == 0) ret = relay_trigger();
    else if (strcmp(action, "pulse") == 0) {
        cJSON *dur = cJSON_GetObjectItem(json, "duration_ms");
        ret = relay_pulse(cJSON_IsNumber(dur) ? (uint32_t)dur->valuedouble : 500);
    } else {
        cJSON_Delete(json);
        return web_server_send_error(req, 400, "Invalid action");
    }

    cJSON_Delete(json);
    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    return web_server_send_error(req, 500, "Relay error");
}

// ============================================================================
// WiFi API
// ============================================================================

static esp_err_t h_api_wifi_status(httpd_req_t *req)
{
    s_request_count++;

    char json[256];
    snprintf(json, sizeof(json),
        "{\"connected\":%s,\"sta_ip\":\"%s\",\"ap_ip\":\"%s\",\"ap_ssid\":\"%s\"}",
        wifi_manager_is_connected() ? "true" : "false",
        wifi_manager_get_ip(),
        wifi_manager_get_ap_ip(),
        wifi_manager_get_ap_ssid());

    return web_server_send_json(req, json);
}

// ============================================================================
// Setup API (auth gerektirmez - ilk kurulum)
// ============================================================================

static esp_err_t h_api_setup_status(httpd_req_t *req)
{
    s_request_count++;
    char json[64];
    snprintf(json, sizeof(json), "{\"setup_completed\":%s}",
             config_is_setup_completed() ? "true" : "false");
    return web_server_send_json(req, json);
}

static esp_err_t h_api_setup_wifi_scan(httpd_req_t *req)
{
    s_request_count++;

    if (wifi_manager_scan() != ESP_OK) {
        return web_server_send_error(req, 500, "Scan failed");
    }

    wifi_ap_record_t recs[WIFI_MAX_SCAN_RESULTS];
    uint16_t count = WIFI_MAX_SCAN_RESULTS;
    wifi_manager_scan_get_results(recs, &count);

    char *json = malloc(2048);
    if (!json) return web_server_send_error(req, 500, "No memory");

    strcpy(json, "{\"networks\":[");
    for (int i = 0; i < count && i < 15; i++) {
        char entry[128];
        snprintf(entry, sizeof(entry),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d}",
            (i > 0) ? "," : "",
            (char *)recs[i].ssid, recs[i].rssi, recs[i].primary);
        strcat(json, entry);
    }
    strcat(json, "]}");

    esp_err_t ret = web_server_send_json(req, json);
    free(json);
    return ret;
}

static esp_err_t h_api_setup_wifi_connect(httpd_req_t *req)
{
    s_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *ssid_j = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_j = cJSON_GetObjectItem(json, "password");
    const char *ssid = cJSON_IsString(ssid_j) ? ssid_j->valuestring : "";
    const char *pass = cJSON_IsString(pass_j) ? pass_j->valuestring : "";

    // Config'e kaydet
    ls_wifi_config_t wcfg = LS_WIFI_CONFIG_DEFAULT();
    strncpy(wcfg.primary_ssid, ssid, MAX_SSID_LEN - 1);
    strncpy(wcfg.primary_password, pass, MAX_PASSWORD_LEN - 1);
    config_save_wifi(&wcfg);

    esp_err_t ret = wifi_manager_connect(ssid, pass);
    cJSON_Delete(json);

    if (ret == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Connect failed");
}

static esp_err_t h_api_setup_password(httpd_req_t *req)
{
    s_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *pw = cJSON_GetObjectItem(json, "password");
    const char *password = cJSON_IsString(pw) ? pw->valuestring : "";

    esp_err_t ret = session_set_initial_password(password);
    cJSON_Delete(json);

    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    if (ret == ESP_ERR_INVALID_STATE) return web_server_send_error(req, 400, "Password already set");
    return web_server_send_error(req, 400, "Password too short");
}

static esp_err_t h_api_setup_complete(httpd_req_t *req)
{
    s_request_count++;
    config_mark_setup_completed();
    return web_server_send_json(req, "{\"success\":true}");
}

// ============================================================================
// Password change API (authenticated)
// ============================================================================

static esp_err_t h_api_password_change(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    // Accept both camelCase (app.js) and snake_case
    cJSON *cur = cJSON_GetObjectItem(json, "currentPassword");
    if (!cur) cur = cJSON_GetObjectItem(json, "current_password");
    cJSON *nw = cJSON_GetObjectItem(json, "newPassword");
    if (!nw) nw = cJSON_GetObjectItem(json, "new_password");
    const char *cur_pw = cJSON_IsString(cur) ? cur->valuestring : "";
    const char *new_pw = cJSON_IsString(nw) ? nw->valuestring : "";

    esp_err_t ret = session_change_password(cur_pw, new_pw);
    cJSON_Delete(json);

    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    if (ret == ESP_ERR_INVALID_ARG) return web_server_send_error(req, 400, "Wrong current password");
    if (ret == ESP_ERR_INVALID_SIZE) return web_server_send_error(req, 400, "Password too short");
    return web_server_send_error(req, 500, "Password change failed");
}

// ============================================================================
// GUI Download API
// ============================================================================

static esp_err_t h_api_gui_download(httpd_req_t *req)
{
    s_request_count++;

    char body[256] = {0};
    const char *repo = NULL, *branch = NULL, *path = NULL;

    if (req->content_len > 0 && (size_t)req->content_len < sizeof(body)) {
        httpd_req_recv(req, body, req->content_len);
        cJSON *json = cJSON_Parse(body);
        if (json) {
            cJSON *it;
            if ((it = cJSON_GetObjectItem(json, "repo")) && cJSON_IsString(it)) repo = it->valuestring;
            if ((it = cJSON_GetObjectItem(json, "branch")) && cJSON_IsString(it)) branch = it->valuestring;
            if ((it = cJSON_GetObjectItem(json, "path")) && cJSON_IsString(it)) path = it->valuestring;
            esp_err_t ret = gui_downloader_start(repo, branch, path);
            cJSON_Delete(json);
            if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
            return web_server_send_error(req, 500, "Download start failed");
        }
    }

    esp_err_t ret = gui_downloader_start(NULL, NULL, NULL);
    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    return web_server_send_error(req, 500, "Download start failed");
}

static esp_err_t h_api_gui_download_status(httpd_req_t *req)
{
    s_request_count++;

    gui_dl_status_t st;
    gui_downloader_get_status(&st);

    const char *snames[] = {"idle", "connecting", "downloading", "installing", "complete", "error"};

    char json[512];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"progress\":%d,\"message\":\"%s\",\"error\":\"%s\","
        "\"bytes_downloaded\":%lu,\"files_downloaded\":%d,\"total_files\":%d}",
        snames[st.state], st.progress, st.message, st.error,
        st.bytes_downloaded, st.files_downloaded, st.total_files);

    return web_server_send_json(req, json);
}

// ============================================================================
// System API
// ============================================================================

static esp_err_t h_api_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    uint32_t total = 0, used = 0;
    file_manager_get_info(&total, &used);

    char json[256];
    snprintf(json, sizeof(json),
        "{\"uptime_ms\":%llu,\"heap_free\":%lu,\"heap_min\":%lu,"
        "\"flash_total\":%lu,\"flash_used\":%lu,\"requests\":%lu}",
        (unsigned long long)(esp_timer_get_time() / 1000),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        total, used, s_request_count);

    return web_server_send_json(req, json);
}

static esp_err_t h_api_reboot(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;
    ESP_LOGW(TAG, "Reboot istegi alindi");

    web_server_send_json(req, "{\"success\":true,\"message\":\"Rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

static esp_err_t h_api_factory_reset(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;
    ESP_LOGW(TAG, "Factory reset istegi alindi");

    config_factory_reset();
    web_server_send_json(req, "{\"success\":true,\"message\":\"Factory reset done, rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ============================================================================
// OTA API
// ============================================================================

static esp_err_t h_api_ota_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    const char *snames[] = {"idle", "downloading", "verifying", "updating", "complete", "error"};

    char json[128];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"progress\":%d,\"version\":\"%s\"}",
        snames[ota_manager_get_state()],
        ota_manager_get_progress(),
        ota_manager_get_current_version());

    return web_server_send_json(req, json);
}

static esp_err_t h_api_ota_url(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *url_j = cJSON_GetObjectItem(json, "url");
    const char *url = cJSON_IsString(url_j) ? url_j->valuestring : NULL;

    if (!url) {
        cJSON_Delete(json);
        return web_server_send_error(req, 400, "Missing 'url'");
    }

    esp_err_t ret = ota_manager_start_from_url(url);
    cJSON_Delete(json);

    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    return web_server_send_error(req, 500, "OTA failed");
}

// ============================================================================
// WiFi Config API
// ============================================================================

static esp_err_t h_api_config_wifi_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    ls_wifi_config_t cfg;
    if (config_load_wifi(&cfg) != ESP_OK) {
        ls_wifi_config_t def = LS_WIFI_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    // app.js expects nested {primary: {...}, backup: {...}, hostname: "..."}
    cJSON *root = cJSON_CreateObject();
    if (!root) return web_server_send_error(req, 500, "No memory");

    cJSON *primary = cJSON_CreateObject();
    cJSON_AddStringToObject(primary, "ssid", cfg.primary_ssid);
    cJSON_AddStringToObject(primary, "password", cfg.primary_password[0] ? "********" : "");
    cJSON_AddBoolToObject(primary, "staticIpEnabled", cfg.primary_static_enabled);
    cJSON_AddStringToObject(primary, "staticIp", cfg.primary_static.ip);
    cJSON_AddStringToObject(primary, "gateway", cfg.primary_static.gateway);
    cJSON_AddStringToObject(primary, "subnet", cfg.primary_static.subnet);
    cJSON_AddStringToObject(primary, "dns", cfg.primary_static.dns);
    cJSON_AddItemToObject(root, "primary", primary);

    cJSON *backup = cJSON_CreateObject();
    cJSON_AddStringToObject(backup, "ssid", cfg.secondary_ssid);
    cJSON_AddStringToObject(backup, "password", cfg.secondary_password[0] ? "********" : "");
    cJSON_AddBoolToObject(backup, "staticIpEnabled", cfg.secondary_static_enabled);
    cJSON_AddStringToObject(backup, "staticIp", cfg.secondary_static.ip);
    cJSON_AddStringToObject(backup, "gateway", cfg.secondary_static.gateway);
    cJSON_AddStringToObject(backup, "subnet", cfg.secondary_static.subnet);
    cJSON_AddStringToObject(backup, "dns", cfg.secondary_static.dns);
    cJSON_AddItemToObject(root, "backup", backup);

    cJSON_AddStringToObject(root, "hostname", cfg.primary_mdns);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

static esp_err_t h_api_config_wifi_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[1024] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    ls_wifi_config_t cfg;
    if (config_load_wifi(&cfg) != ESP_OK) {
        ls_wifi_config_t def = LS_WIFI_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    // app.js sends: {type: "primary"/"backup", ssid, password, staticIpEnabled, staticIp, gateway, subnet, dns, mdnsHostname}
    cJSON *target = cJSON_GetObjectItem(json, "type");
    if (!target) target = cJSON_GetObjectItem(json, "target");
    const char *tgt = cJSON_IsString(target) ? target->valuestring : "primary";

    cJSON *it;
    if (strcmp(tgt, "backup") == 0 || strcmp(tgt, "secondary") == 0) {
        if ((it = cJSON_GetObjectItem(json, "ssid")) && cJSON_IsString(it))
            strncpy(cfg.secondary_ssid, it->valuestring, MAX_SSID_LEN - 1);
        if ((it = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(it)) {
            if (strcmp(it->valuestring, "********") != 0)
                strncpy(cfg.secondary_password, it->valuestring, MAX_PASSWORD_LEN - 1);
        }
        if ((it = cJSON_GetObjectItem(json, "staticIpEnabled")))
            cfg.secondary_static_enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(json, "staticIp")) && cJSON_IsString(it))
            strncpy(cfg.secondary_static.ip, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "gateway")) && cJSON_IsString(it))
            strncpy(cfg.secondary_static.gateway, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "subnet")) && cJSON_IsString(it))
            strncpy(cfg.secondary_static.subnet, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "dns")) && cJSON_IsString(it))
            strncpy(cfg.secondary_static.dns, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "mdnsHostname")) && cJSON_IsString(it))
            strncpy(cfg.secondary_mdns, it->valuestring, MAX_HOSTNAME_LEN - 1);
    } else {
        if ((it = cJSON_GetObjectItem(json, "ssid")) && cJSON_IsString(it))
            strncpy(cfg.primary_ssid, it->valuestring, MAX_SSID_LEN - 1);
        if ((it = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(it)) {
            if (strcmp(it->valuestring, "********") != 0)
                strncpy(cfg.primary_password, it->valuestring, MAX_PASSWORD_LEN - 1);
        }
        if ((it = cJSON_GetObjectItem(json, "staticIpEnabled")))
            cfg.primary_static_enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(json, "staticIp")) && cJSON_IsString(it))
            strncpy(cfg.primary_static.ip, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "gateway")) && cJSON_IsString(it))
            strncpy(cfg.primary_static.gateway, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "subnet")) && cJSON_IsString(it))
            strncpy(cfg.primary_static.subnet, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "dns")) && cJSON_IsString(it))
            strncpy(cfg.primary_static.dns, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "mdnsHostname")) && cJSON_IsString(it))
            strncpy(cfg.primary_mdns, it->valuestring, MAX_HOSTNAME_LEN - 1);
    }

    if ((it = cJSON_GetObjectItem(json, "ap_mode_enabled")))
        cfg.ap_mode_enabled = cJSON_IsTrue(it);

    cJSON_Delete(json);

    if (config_save_wifi(&cfg) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// Relay Config API
// ============================================================================

static esp_err_t h_api_config_relay_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    ls_relay_config_t cfg;
    if (config_load_relay(&cfg) != ESP_OK) {
        ls_relay_config_t def = LS_RELAY_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    // app.js expects: inverted, pulseMode, pulseDurationMs, onDelayMs, offDelayMs
    char json[256];
    snprintf(json, sizeof(json),
        "{\"inverted\":%s,"
        "\"pulseMode\":%s,"
        "\"pulseDurationMs\":%lu,"
        "\"pulseIntervalMs\":%lu,"
        "\"onDelayMs\":%lu,"
        "\"offDelayMs\":%lu}",
        cfg.inverted ? "true" : "false",
        cfg.pulse_enabled ? "true" : "false",
        (unsigned long)(cfg.pulse_on_ms),
        (unsigned long)(cfg.pulse_off_ms),
        (unsigned long)(cfg.delay_seconds * 1000UL),
        (unsigned long)(cfg.duration_seconds * 1000UL));

    return web_server_send_json(req, json);
}

static esp_err_t h_api_config_relay_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    ls_relay_config_t cfg;
    config_load_relay(&cfg);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "inverted"))) cfg.inverted = cJSON_IsTrue(it);

    // app.js sends: pulseMode, pulseDurationMs, pulseIntervalMs, onDelayMs, offDelayMs
    if ((it = cJSON_GetObjectItem(json, "pulseMode"))) cfg.pulse_enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(json, "pulseDurationMs"))) cfg.pulse_on_ms = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "pulseIntervalMs"))) cfg.pulse_off_ms = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "onDelayMs"))) cfg.delay_seconds = (uint32_t)(it->valuedouble / 1000.0);
    if ((it = cJSON_GetObjectItem(json, "offDelayMs"))) cfg.duration_seconds = (uint32_t)(it->valuedouble / 1000.0);

    // Also accept legacy field names
    if ((it = cJSON_GetObjectItem(json, "delay_seconds"))) cfg.delay_seconds = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "duration_seconds"))) cfg.duration_seconds = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "pulse_enabled"))) cfg.pulse_enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(json, "pulse_on_ms"))) cfg.pulse_on_ms = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "pulse_off_ms"))) cfg.pulse_off_ms = (uint32_t)it->valuedouble;

    cJSON_Delete(json);

    if (config_save_relay(&cfg) == ESP_OK) {
        // Apply config to relay manager
        relay_config_t rcfg = {
            .inverted = cfg.inverted,
            .delay_seconds = cfg.delay_seconds,
            .duration_seconds = cfg.duration_seconds,
            .pulse_enabled = cfg.pulse_enabled,
            .pulse_on_ms = cfg.pulse_on_ms,
            .pulse_off_ms = cfg.pulse_off_ms
        };
        relay_set_config(&rcfg);
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// Mail Groups API
// ============================================================================

static esp_err_t h_api_config_mail_groups_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    // app.js expects {groups: [{name, subject, content, recipients}, ...]}
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        if (root) cJSON_Delete(root);
        if (arr) cJSON_Delete(arr);
        return web_server_send_error(req, 500, "No memory");
    }

    for (int i = 0; i < MAX_MAIL_GROUPS; i++) {
        mail_group_t grp;
        if (config_load_mail_group(i, &grp) != ESP_OK) {
            mail_group_t def = MAIL_GROUP_DEFAULT();
            memcpy(&grp, &def, sizeof(grp));
        }
        // Skip empty groups
        if (grp.name[0] == '\0' && grp.recipient_count == 0) continue;

        cJSON *obj = cJSON_CreateObject();
        if (!obj) continue;

        cJSON_AddStringToObject(obj, "name", grp.name);
        cJSON_AddStringToObject(obj, "subject", "");
        cJSON_AddStringToObject(obj, "content", "");

        cJSON *recips = cJSON_CreateArray();
        if (recips) {
            for (int r = 0; r < grp.recipient_count && r < MAX_RECIPIENTS; r++) {
                cJSON_AddItemToArray(recips, cJSON_CreateString(grp.recipients[r]));
            }
            cJSON_AddItemToObject(obj, "recipients", recips);
        }

        cJSON_AddItemToArray(arr, obj);
    }

    cJSON_AddItemToObject(root, "groups", arr);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

static esp_err_t h_api_config_mail_groups_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[1024] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *idx_j = cJSON_GetObjectItem(json, "index");
    if (!cJSON_IsNumber(idx_j) || idx_j->valueint < 0 || idx_j->valueint >= MAX_MAIL_GROUPS) {
        cJSON_Delete(json);
        return web_server_send_error(req, 400, "Invalid index");
    }
    int idx = idx_j->valueint;

    mail_group_t grp;
    config_load_mail_group(idx, &grp);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "name")) && cJSON_IsString(it))
        strncpy(grp.name, it->valuestring, MAX_GROUP_NAME_LEN - 1);
    if ((it = cJSON_GetObjectItem(json, "enabled")))
        grp.enabled = cJSON_IsTrue(it);

    cJSON *recips = cJSON_GetObjectItem(json, "recipients");
    if (cJSON_IsArray(recips)) {
        grp.recipient_count = 0;
        int count = cJSON_GetArraySize(recips);
        for (int r = 0; r < count && r < MAX_RECIPIENTS; r++) {
            cJSON *email = cJSON_GetArrayItem(recips, r);
            if (cJSON_IsString(email)) {
                strncpy(grp.recipients[r], email->valuestring, MAX_EMAIL_LEN - 1);
                grp.recipient_count++;
            }
        }
    }

    cJSON_Delete(json);

    if (config_save_mail_group(idx, &grp) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// Security Config API
// ============================================================================

static esp_err_t h_api_config_security_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    auth_config_t auth;
    if (config_load_auth(&auth) != ESP_OK) {
        auth_config_t def = AUTH_CONFIG_DEFAULT();
        memcpy(&auth, &def, sizeof(auth));
    }

    api_config_t api;
    if (config_load_api(&api) != ESP_OK) {
        api_config_t def = API_CONFIG_DEFAULT();
        memcpy(&api, &def, sizeof(api));
    }

    // app.js expects: loginProtection, lockoutTime, resetApiEnabled, apiKey
    char json[512];
    snprintf(json, sizeof(json),
        "{\"loginProtection\":true,"
        "\"lockoutTime\":15,"
        "\"resetApiEnabled\":%s,"
        "\"apiKey\":\"%s\","
        "\"sessionTimeoutMin\":%lu}",
        api.enabled ? "true" : "false",
        api.token[0] ? api.token : "",
        (unsigned long)auth.session_timeout_min);

    return web_server_send_json(req, json);
}

static esp_err_t h_api_config_security_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    auth_config_t auth;
    config_load_auth(&auth);
    api_config_t api;
    config_load_api(&api);

    cJSON *it;
    // app.js sends: loginProtection, lockoutTime, resetApiEnabled
    if ((it = cJSON_GetObjectItem(json, "resetApiEnabled")))
        api.enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(json, "sessionTimeoutMin")))
        auth.session_timeout_min = (uint32_t)it->valuedouble;
    // Also accept legacy field names
    if ((it = cJSON_GetObjectItem(json, "session_timeout_min")))
        auth.session_timeout_min = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "api_enabled")))
        api.enabled = cJSON_IsTrue(it);

    cJSON_Delete(json);

    bool ok = true;
    if (config_save_auth(&auth) != ESP_OK) ok = false;
    if (config_save_api(&api) != ESP_OK) ok = false;

    if (ok) return web_server_send_json(req, "{\"success\":true}");
    return web_server_send_error(req, 500, "Save failed");
}

static esp_err_t h_api_config_security_apikey(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    api_config_t api;
    if (config_load_api(&api) != ESP_OK) {
        api_config_t def = API_CONFIG_DEFAULT();
        memcpy(&api, &def, sizeof(api));
    }

    // Generate 16 random bytes, hex-encode to 32 char key
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));

    char key[33];
    for (int i = 0; i < 16; i++) {
        snprintf(&key[i * 2], 3, "%02x", rnd[i]);
    }
    key[32] = '\0';

    strncpy(api.token, key, MAX_TOKEN_LEN - 1);
    api.token[MAX_TOKEN_LEN - 1] = '\0';

    if (config_save_api(&api) == ESP_OK) {
        // app.js expects "apiKey" field
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"success\":true,\"apiKey\":\"%s\"}", key);
        return web_server_send_json(req, resp);
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// AP Mode Config API
// ============================================================================

static esp_err_t h_api_config_ap(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    ls_wifi_config_t cfg;
    if (config_load_wifi(&cfg) != ESP_OK) {
        ls_wifi_config_t def = LS_WIFI_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    // Accept both "enabled" (app.js) and "ap_mode_enabled"
    cJSON *it = cJSON_GetObjectItem(json, "enabled");
    if (!it) it = cJSON_GetObjectItem(json, "ap_mode_enabled");
    if (it) cfg.ap_mode_enabled = cJSON_IsTrue(it);

    cJSON_Delete(json);

    if (config_save_wifi(&cfg) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// Config Export/Import API
// ============================================================================

static esp_err_t h_api_config_export(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    cJSON *root = cJSON_CreateObject();
    if (!root) return web_server_send_error(req, 500, "No memory");

    // Timer config
    timer_config_t timer;
    if (config_load_timer(&timer) == ESP_OK) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddBoolToObject(t, "enabled", timer.enabled);
        cJSON_AddNumberToObject(t, "interval_hours", timer.interval_hours);
        cJSON_AddNumberToObject(t, "warning_minutes", timer.warning_minutes);
        cJSON_AddStringToObject(t, "check_start", timer.check_start);
        cJSON_AddStringToObject(t, "check_end", timer.check_end);
        cJSON_AddStringToObject(t, "relay_action", timer.relay_action);
        cJSON_AddItemToObject(root, "timer", t);
    }

    // WiFi config
    ls_wifi_config_t wifi;
    if (config_load_wifi(&wifi) == ESP_OK) {
        cJSON *w = cJSON_CreateObject();
        cJSON_AddStringToObject(w, "primary_ssid", wifi.primary_ssid);
        cJSON_AddStringToObject(w, "primary_password", wifi.primary_password);
        cJSON_AddStringToObject(w, "secondary_ssid", wifi.secondary_ssid);
        cJSON_AddStringToObject(w, "secondary_password", wifi.secondary_password);
        cJSON_AddBoolToObject(w, "ap_mode_enabled", wifi.ap_mode_enabled);
        cJSON_AddItemToObject(root, "wifi", w);
    }

    // Mail config
    mail_config_t mail;
    if (config_load_mail(&mail) == ESP_OK) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "server", mail.server);
        cJSON_AddNumberToObject(m, "port", mail.port);
        cJSON_AddStringToObject(m, "username", mail.username);
        cJSON_AddStringToObject(m, "password", mail.password);
        cJSON_AddStringToObject(m, "sender_name", mail.sender_name);
        cJSON_AddItemToObject(root, "mail", m);
    }

    // Mail groups
    cJSON *groups = cJSON_CreateArray();
    for (int i = 0; i < MAX_MAIL_GROUPS; i++) {
        mail_group_t grp;
        if (config_load_mail_group(i, &grp) == ESP_OK) {
            cJSON *g = cJSON_CreateObject();
            cJSON_AddStringToObject(g, "name", grp.name);
            cJSON_AddBoolToObject(g, "enabled", grp.enabled);
            cJSON_AddNumberToObject(g, "recipient_count", grp.recipient_count);
            cJSON *recips = cJSON_CreateArray();
            for (int r = 0; r < grp.recipient_count && r < MAX_RECIPIENTS; r++) {
                cJSON_AddItemToArray(recips, cJSON_CreateString(grp.recipients[r]));
            }
            cJSON_AddItemToObject(g, "recipients", recips);
            cJSON_AddItemToArray(groups, g);
        }
    }
    cJSON_AddItemToObject(root, "mail_groups", groups);

    // Relay config
    ls_relay_config_t relay;
    if (config_load_relay(&relay) == ESP_OK) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "inverted", relay.inverted);
        cJSON_AddNumberToObject(r, "delay_seconds", relay.delay_seconds);
        cJSON_AddNumberToObject(r, "duration_seconds", relay.duration_seconds);
        cJSON_AddBoolToObject(r, "pulse_enabled", relay.pulse_enabled);
        cJSON_AddNumberToObject(r, "pulse_on_ms", relay.pulse_on_ms);
        cJSON_AddNumberToObject(r, "pulse_off_ms", relay.pulse_off_ms);
        cJSON_AddItemToObject(root, "relay", r);
    }

    // API config
    api_config_t api;
    if (config_load_api(&api) == ESP_OK) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddBoolToObject(a, "enabled", api.enabled);
        cJSON_AddStringToObject(a, "endpoint", api.endpoint);
        cJSON_AddBoolToObject(a, "require_token", api.require_token);
        cJSON_AddStringToObject(a, "token", api.token);
        cJSON_AddItemToObject(root, "api", a);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

static esp_err_t h_api_config_import(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    // Allocate larger buffer for import
    char *body = malloc(4096);
    if (!body) return web_server_send_error(req, 500, "No memory");

    if (read_body(req, body, 4096) < 0) {
        free(body);
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *it, *sub;

    // Timer config
    sub = cJSON_GetObjectItem(root, "timer");
    if (cJSON_IsObject(sub)) {
        timer_config_t cfg;
        config_load_timer(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "enabled"))) cfg.enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "interval_hours"))) cfg.interval_hours = it->valueint;
        if ((it = cJSON_GetObjectItem(sub, "warning_minutes"))) cfg.warning_minutes = it->valueint;
        if ((it = cJSON_GetObjectItem(sub, "check_start")) && cJSON_IsString(it))
            strncpy(cfg.check_start, it->valuestring, sizeof(cfg.check_start) - 1);
        if ((it = cJSON_GetObjectItem(sub, "check_end")) && cJSON_IsString(it))
            strncpy(cfg.check_end, it->valuestring, sizeof(cfg.check_end) - 1);
        if ((it = cJSON_GetObjectItem(sub, "relay_action")) && cJSON_IsString(it))
            strncpy(cfg.relay_action, it->valuestring, sizeof(cfg.relay_action) - 1);
        config_save_timer(&cfg);
    }

    // WiFi config
    sub = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(sub)) {
        ls_wifi_config_t cfg;
        config_load_wifi(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "primary_ssid")) && cJSON_IsString(it))
            strncpy(cfg.primary_ssid, it->valuestring, MAX_SSID_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "primary_password")) && cJSON_IsString(it))
            strncpy(cfg.primary_password, it->valuestring, MAX_PASSWORD_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "secondary_ssid")) && cJSON_IsString(it))
            strncpy(cfg.secondary_ssid, it->valuestring, MAX_SSID_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "secondary_password")) && cJSON_IsString(it))
            strncpy(cfg.secondary_password, it->valuestring, MAX_PASSWORD_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "ap_mode_enabled")))
            cfg.ap_mode_enabled = cJSON_IsTrue(it);
        config_save_wifi(&cfg);
    }

    // Mail config
    sub = cJSON_GetObjectItem(root, "mail");
    if (cJSON_IsObject(sub)) {
        mail_config_t cfg;
        config_load_mail(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "server")) && cJSON_IsString(it))
            strncpy(cfg.server, it->valuestring, sizeof(cfg.server) - 1);
        if ((it = cJSON_GetObjectItem(sub, "port")))
            cfg.port = it->valueint;
        if ((it = cJSON_GetObjectItem(sub, "username")) && cJSON_IsString(it))
            strncpy(cfg.username, it->valuestring, sizeof(cfg.username) - 1);
        if ((it = cJSON_GetObjectItem(sub, "password")) && cJSON_IsString(it))
            strncpy(cfg.password, it->valuestring, sizeof(cfg.password) - 1);
        if ((it = cJSON_GetObjectItem(sub, "sender_name")) && cJSON_IsString(it))
            strncpy(cfg.sender_name, it->valuestring, sizeof(cfg.sender_name) - 1);
        config_save_mail(&cfg);
    }

    // Mail groups
    cJSON *groups = cJSON_GetObjectItem(root, "mail_groups");
    if (cJSON_IsArray(groups)) {
        int count = cJSON_GetArraySize(groups);
        for (int i = 0; i < count && i < MAX_MAIL_GROUPS; i++) {
            cJSON *g = cJSON_GetArrayItem(groups, i);
            if (!cJSON_IsObject(g)) continue;

            mail_group_t grp = MAIL_GROUP_DEFAULT();
            if ((it = cJSON_GetObjectItem(g, "name")) && cJSON_IsString(it))
                strncpy(grp.name, it->valuestring, MAX_GROUP_NAME_LEN - 1);
            if ((it = cJSON_GetObjectItem(g, "enabled")))
                grp.enabled = cJSON_IsTrue(it);

            cJSON *recips = cJSON_GetObjectItem(g, "recipients");
            if (cJSON_IsArray(recips)) {
                int rc = cJSON_GetArraySize(recips);
                for (int r = 0; r < rc && r < MAX_RECIPIENTS; r++) {
                    cJSON *email = cJSON_GetArrayItem(recips, r);
                    if (cJSON_IsString(email)) {
                        strncpy(grp.recipients[r], email->valuestring, MAX_EMAIL_LEN - 1);
                        grp.recipient_count++;
                    }
                }
            }
            config_save_mail_group(i, &grp);
        }
    }

    // Relay config
    sub = cJSON_GetObjectItem(root, "relay");
    if (cJSON_IsObject(sub)) {
        ls_relay_config_t cfg;
        config_load_relay(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "inverted"))) cfg.inverted = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "delay_seconds"))) cfg.delay_seconds = (uint32_t)it->valuedouble;
        if ((it = cJSON_GetObjectItem(sub, "duration_seconds"))) cfg.duration_seconds = (uint32_t)it->valuedouble;
        if ((it = cJSON_GetObjectItem(sub, "pulse_enabled"))) cfg.pulse_enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "pulse_on_ms"))) cfg.pulse_on_ms = (uint32_t)it->valuedouble;
        if ((it = cJSON_GetObjectItem(sub, "pulse_off_ms"))) cfg.pulse_off_ms = (uint32_t)it->valuedouble;
        config_save_relay(&cfg);
    }

    // API config
    sub = cJSON_GetObjectItem(root, "api");
    if (cJSON_IsObject(sub)) {
        api_config_t cfg;
        config_load_api(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "enabled"))) cfg.enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "endpoint")) && cJSON_IsString(it))
            strncpy(cfg.endpoint, it->valuestring, MAX_HOSTNAME_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "require_token"))) cfg.require_token = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "token")) && cJSON_IsString(it))
            strncpy(cfg.token, it->valuestring, MAX_TOKEN_LEN - 1);
        config_save_api(&cfg);
    }

    cJSON_Delete(root);
    return web_server_send_json(req, "{\"success\":true}");
}

// ============================================================================
// Timer Control API
// ============================================================================

static esp_err_t h_api_timer_enable(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    if (timer_set_enabled(true) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Timer enable failed");
}

static esp_err_t h_api_timer_disable(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    if (timer_set_enabled(false) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Timer disable failed");
}

static esp_err_t h_api_timer_acknowledge(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    if (timer_reset() == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Timer acknowledge failed");
}

static esp_err_t h_api_timer_vacation(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    cJSON *days = cJSON_GetObjectItem(json, "days");

    timer_config_t cfg;
    config_load_timer(&cfg);

    // If vacation enabled, disable timer; if vacation disabled, re-enable
    if (cJSON_IsTrue(enabled)) {
        cfg.enabled = false;
    } else {
        cfg.enabled = true;
    }

    cJSON_Delete(json);

    if (config_save_timer(&cfg) == ESP_OK) {
        timer_set_enabled(cfg.enabled);
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// Relay Test API
// ============================================================================

static esp_err_t h_api_relay_test(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    if (relay_pulse(500) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Relay test failed");
}

// ============================================================================
// OTA Check API
// ============================================================================

static esp_err_t h_api_ota_check(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    // app.js expects camelCase: updateAvailable, currentVersion
    char json[256];
    snprintf(json, sizeof(json),
        "{\"currentVersion\":\"%s\","
        "\"updateAvailable\":false,"
        "\"version\":\"\"}",
        ota_manager_get_current_version());

    return web_server_send_json(req, json);
}

// ============================================================================
// Logs API
// ============================================================================

static esp_err_t h_api_logs_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    // app.js expects {entries: [{timestamp, category, message}, ...]}
    cJSON *root = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    if (!root || !entries) {
        if (root) cJSON_Delete(root);
        if (entries) cJSON_Delete(entries);
        return web_server_send_error(req, 500, "No memory");
    }

    char files[LOG_MGR_MAX_FILES][64];
    size_t count = 0;

    if (log_manager_list_files(files, LOG_MGR_MAX_FILES, &count) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            char *buf = malloc(4096);
            if (!buf) continue;

            size_t read_size = 0;
            if (log_manager_read_file(files[i], buf, 4096, &read_size) == ESP_OK) {
                buf[read_size] = '\0';
                // Parse log lines: each line is a log entry
                char *line = strtok(buf, "\n");
                while (line) {
                    if (strlen(line) > 0) {
                        cJSON *entry = cJSON_CreateObject();
                        if (entry) {
                            // Use current time as timestamp approximation
                            cJSON_AddNumberToObject(entry, "timestamp",
                                (double)(time(NULL) - (count - i) * 3600));
                            cJSON_AddStringToObject(entry, "category", "system");
                            cJSON_AddStringToObject(entry, "message", line);
                            cJSON_AddItemToArray(entries, entry);
                        }
                    }
                    line = strtok(NULL, "\n");
                }
            }
            free(buf);
        }
    }

    cJSON_AddItemToObject(root, "entries", entries);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

static esp_err_t h_api_logs_delete(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    if (log_manager_clear_all() == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Clear logs failed");
}

// ============================================================================
// SMTP Test API
// ============================================================================

static esp_err_t h_api_test_smtp(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    s_request_count++;

    mail_result_t result;
    esp_err_t ret = mail_test_connection(&result);

    char json[256];
    if (ret == ESP_OK && result.success) {
        snprintf(json, sizeof(json),
            "{\"success\":true,\"smtp_code\":%d,\"message\":\"Connection OK\"}",
            result.smtp_code);
    } else {
        snprintf(json, sizeof(json),
            "{\"success\":false,\"smtp_code\":%d,\"error\":\"%s\"}",
            result.smtp_code, result.error_msg);
    }

    return web_server_send_json(req, json);
}

// ============================================================================
// 404 handler
// ============================================================================

static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    s_request_count++;

    // Statik dosya olabilir mi? (auth + dosya kontrol)
    const char *uri = req->uri;

    // Setup kontrolu
    if (!config_is_setup_completed()) {
        if (strncmp(uri, "/api/setup/", 11) != 0 &&
            strcmp(uri, "/setup.html") != 0) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/setup.html");
            return httpd_resp_send(req, NULL, 0);
        }
    }

    // login.html auth gerektirmez, diger sayfalar gerektirir
    if (strcmp(uri, "/login.html") != 0 && strncmp(uri, "/api/", 5) != 0) {
        if (!check_auth(req)) {
            return send_unauthorized(req);
        }
    }

    // Dosya yolu olustur
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s%s", FILE_MGR_WEB_PATH, uri);

    // Path traversal engelle
    if (strstr(filepath, "..")) {
        return web_server_send_error(req, 403, "Forbidden");
    }

    // Dosya var mi?
    if (file_manager_exists(filepath)) {
        httpd_resp_set_type(req, get_mime(filepath));
        return web_server_send_file(req, filepath);
    }

    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"not_found\"}", HTTPD_RESP_USE_STRLEN);
}

// ============================================================================
// URI kayit makrosu
// ============================================================================

#define REG(path, method, handler) do { \
    httpd_uri_t u = {.uri = path, .method = method, .handler = handler}; \
    httpd_register_uri_handler(s_server, &u); \
} while(0)

// ============================================================================
// Public API
// ============================================================================

esp_err_t web_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Sunucu zaten calisiyor");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.stack_size = WEB_SERVER_STACK_SIZE;
    config.max_uri_handlers = WEB_SERVER_MAX_URI;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sunucu baslatilamadi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Sayfalar
    REG("/", HTTP_GET, h_index);
    REG("/login.html", HTTP_GET, h_login_page);
    REG("/setup.html", HTTP_GET, h_setup_page);

    // Auth API
    REG("/api/login", HTTP_POST, h_api_login);
    REG("/api/logout", HTTP_POST, h_api_logout);

    // Timer API
    REG("/api/timer/status", HTTP_GET, h_api_timer_status);
    REG("/api/timer/reset", HTTP_POST, h_api_timer_reset);
    REG("/api/config/timer", HTTP_GET, h_api_config_timer_get);
    REG("/api/config/timer", HTTP_POST, h_api_config_timer_post);

    // Mail API
    REG("/api/config/mail", HTTP_GET, h_api_config_mail_get);
    REG("/api/config/mail", HTTP_POST, h_api_config_mail_post);
    REG("/api/mail/test", HTTP_POST, h_api_mail_test);
    REG("/api/mail/stats", HTTP_GET, h_api_mail_stats);

    // Device API
    REG("/api/device/info", HTTP_GET, h_api_device_info);
    REG("/api/status", HTTP_GET, h_api_status);

    // Relay API
    REG("/api/relay/status", HTTP_GET, h_api_relay_status);
    REG("/api/relay/control", HTTP_POST, h_api_relay_control);

    // WiFi API
    REG("/api/wifi/status", HTTP_GET, h_api_wifi_status);

    // Setup API (auth gerektirmez)
    REG("/api/setup/status", HTTP_GET, h_api_setup_status);
    REG("/api/setup/wifi/scan", HTTP_GET, h_api_setup_wifi_scan);
    REG("/api/setup/wifi/connect", HTTP_POST, h_api_setup_wifi_connect);
    REG("/api/setup/password", HTTP_POST, h_api_setup_password);
    REG("/api/setup/complete", HTTP_POST, h_api_setup_complete);

    // Password change
    REG("/api/config/password", HTTP_POST, h_api_password_change);

    // GUI Download API
    REG("/api/gui/download", HTTP_POST, h_api_gui_download);
    REG("/api/gui/download/status", HTTP_GET, h_api_gui_download_status);

    // System API
    REG("/api/reboot", HTTP_POST, h_api_reboot);
    REG("/api/factory-reset", HTTP_POST, h_api_factory_reset);

    // OTA API
    REG("/api/ota/status", HTTP_GET, h_api_ota_status);
    REG("/api/ota/url", HTTP_POST, h_api_ota_url);
    REG("/api/ota/check", HTTP_GET, h_api_ota_check);

    // WiFi Config API
    REG("/api/config/wifi", HTTP_GET, h_api_config_wifi_get);
    REG("/api/config/wifi", HTTP_POST, h_api_config_wifi_post);

    // SMTP Config API (GUI camelCase field names)
    REG("/api/config/smtp", HTTP_GET, h_api_config_smtp_get);
    REG("/api/config/smtp", HTTP_POST, h_api_config_smtp_post);

    // Relay Config API
    REG("/api/config/relay", HTTP_GET, h_api_config_relay_get);
    REG("/api/config/relay", HTTP_POST, h_api_config_relay_post);

    // Mail Groups API
    REG("/api/config/mail-groups", HTTP_GET, h_api_config_mail_groups_get);
    REG("/api/config/mail-groups", HTTP_POST, h_api_config_mail_groups_post);

    // Security Config API
    REG("/api/config/security", HTTP_GET, h_api_config_security_get);
    REG("/api/config/security", HTTP_POST, h_api_config_security_post);
    REG("/api/config/security/api-key", HTTP_POST, h_api_config_security_apikey);

    // AP Mode Config API
    REG("/api/config/ap", HTTP_POST, h_api_config_ap);

    // Config Export/Import API (app.js uses POST for export)
    REG("/api/config/export", HTTP_GET, h_api_config_export);
    REG("/api/config/export", HTTP_POST, h_api_config_export);
    REG("/api/config/import", HTTP_POST, h_api_config_import);

    // Timer Control API
    REG("/api/timer/enable", HTTP_POST, h_api_timer_enable);
    REG("/api/timer/disable", HTTP_POST, h_api_timer_disable);
    REG("/api/timer/acknowledge", HTTP_POST, h_api_timer_acknowledge);
    REG("/api/timer/vacation", HTTP_POST, h_api_timer_vacation);

    // Relay Test API
    REG("/api/relay/test", HTTP_POST, h_api_relay_test);

    // Logs API
    REG("/api/logs", HTTP_GET, h_api_logs_get);
    REG("/api/logs", HTTP_DELETE, h_api_logs_delete);

    // SMTP Test API
    REG("/api/test/smtp", HTTP_POST, h_api_test_smtp);

    // 404 handler (statik dosya fallback)
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_404);

    ESP_LOGI(TAG, "OK - port %d, %lu URI", WEB_SERVER_PORT, (unsigned long)WEB_SERVER_MAX_URI);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) return ESP_OK;
    esp_err_t ret = httpd_stop(s_server);
    if (ret == ESP_OK) {
        s_server = NULL;
        ESP_LOGI(TAG, "Sunucu durduruldu");
    }
    return ret;
}

bool web_server_is_running(void)
{
    return s_server != NULL;
}

esp_err_t web_server_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_send_error(httpd_req_t *req, int status_code, const char *message)
{
    char status[32];
    snprintf(status, sizeof(status), "%d %s", status_code,
             status_code == 400 ? "Bad Request" :
             status_code == 401 ? "Unauthorized" :
             status_code == 403 ? "Forbidden" :
             status_code == 404 ? "Not Found" :
             status_code == 500 ? "Internal Server Error" : "Error");
    httpd_resp_set_status(req, status);

    char json[256];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", message);
    return web_server_send_json(req, json);
}

esp_err_t web_server_send_file(httpd_req_t *req, const char *filepath)
{
    if (!file_manager_exists(filepath)) {
        return web_server_send_error(req, 404, "File not found");
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return web_server_send_error(req, 500, "Cannot open file");
    }

    httpd_resp_set_type(req, get_mime(filepath));

    char *chunk = malloc(1024);
    if (!chunk) {
        fclose(f);
        return web_server_send_error(req, 500, "No memory");
    }

    size_t rd;
    do {
        rd = fread(chunk, 1, 1024, f);
        if (rd > 0) {
            if (httpd_resp_send_chunk(req, chunk, rd) != ESP_OK) {
                fclose(f);
                free(chunk);
                return ESP_FAIL;
            }
        }
    } while (rd > 0);

    fclose(f);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

void web_server_print_stats(void)
{
    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ Durum:     %s", s_server ? "CALISIYOR" : "DURMUS");
    ESP_LOGI(TAG, "│ Port:      %d", WEB_SERVER_PORT);
    ESP_LOGI(TAG, "│ Protokol:  HTTP");
    ESP_LOGI(TAG, "│ Istek:     %lu", s_request_count);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
