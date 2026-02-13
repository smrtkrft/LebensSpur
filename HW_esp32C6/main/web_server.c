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

    char json[512];
    snprintf(json, sizeof(json),
        "{\"state\":%d,"
        "\"remaining_seconds\":%lu,"
        "\"in_active_hours\":%s,"
        "\"reset_count\":%lu,"
        "\"warning_count\":%lu,"
        "\"trigger_count\":%lu}",
        st.state,
        st.remaining_seconds,
        st.in_active_hours ? "true" : "false",
        st.reset_count,
        st.warning_count,
        st.trigger_count);

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

    char json[512];
    snprintf(json, sizeof(json),
        "{\"enabled\":%s,"
        "\"interval_hours\":%lu,"
        "\"warning_minutes\":%lu,"
        "\"check_start\":\"%s\","
        "\"check_end\":\"%s\","
        "\"relay_action\":\"%s\"}",
        cfg.enabled ? "true" : "false",
        cfg.interval_hours,
        cfg.warning_minutes,
        cfg.check_start, cfg.check_end, cfg.relay_action);

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
    if ((it = cJSON_GetObjectItem(json, "interval_hours"))) cfg.interval_hours = it->valueint;
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
// Device Info API
// ============================================================================

static esp_err_t h_api_device_info(httpd_req_t *req)
{
    // Setup modunda auth gerekmez
    if (config_is_setup_completed() && !check_auth(req)) {
        return send_unauthorized(req);
    }
    s_request_count++;

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint32_t flash_total = 0, flash_used = 0;
    file_manager_get_info(&flash_total, &flash_used);

    char json[640];
    snprintf(json, sizeof(json),
        "{"
        "\"device_id\":\"%s\","
        "\"firmware_version\":\"%s\","
        "\"chip\":\"ESP32-C6\","
        "\"cores\":%d,"
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"heap_free\":%lu,"
        "\"heap_min\":%lu,"
        "\"flash_total\":%lu,"
        "\"flash_used\":%lu,"
        "\"uptime_ms\":%llu,"
        "\"wifi_connected\":%s,"
        "\"sta_ip\":\"%s\","
        "\"ap_ip\":\"%s\","
        "\"ap_ssid\":\"%s\""
        "}",
        device_id_get(),
        ota_manager_get_current_version(),
        chip.cores,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        flash_total, flash_used,
        (unsigned long long)(esp_timer_get_time() / 1000),
        wifi_manager_is_connected() ? "true" : "false",
        wifi_manager_get_ip(),
        wifi_manager_get_ap_ip(),
        wifi_manager_get_ap_ssid());

    return web_server_send_json(req, json);
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

    cJSON *cur = cJSON_GetObjectItem(json, "current_password");
    cJSON *nw = cJSON_GetObjectItem(json, "new_password");
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
