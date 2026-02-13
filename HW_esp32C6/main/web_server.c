/**
 * Web Server - HTTP Router ve Statik Dosya Servisi
 *
 * Bearer token + Cookie fallback ile kimlik dogrulama.
 * API handler'lari domain bazli api_*.c dosyalarinda.
 * Bu dosya: yardimci fonksiyonlar, sayfa handler'lari, route kayitlari.
 */

#include "web_server.h"
#include "web_server_internal.h"
#include "file_manager.h"
#include "web_assets.h"
#include "session_auth.h"
#include "config_manager.h"
#include "gui_downloader.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <string.h>

// API handler header'lari
#include "api_auth.h"
#include "api_timer.h"
#include "api_mail.h"
#include "api_device.h"
#include "api_relay.h"
#include "api_wifi.h"
#include "api_setup.h"
#include "api_config.h"
#include "api_ota.h"
#include "api_logs.h"

static const char *TAG = "WEB";

static httpd_handle_t s_server = NULL;
uint32_t ws_request_count = 0;

// ============================================================================
// Auth yardimcilari (api_*.c dosyalarindan da kullanilir)
// ============================================================================

bool check_auth(httpd_req_t *req)
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

esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
}

int read_body(httpd_req_t *req, char *buf, size_t max_len)
{
    int len = req->content_len;
    if (len <= 0 || (size_t)len >= max_len) return -1;
    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) return -1;
    buf[received] = '\0';
    return received;
}

const char *get_mime(const char *path)
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

static esp_err_t h_index(httpd_req_t *req)
{
    ws_request_count++;

    if (!config_is_setup_completed()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup.html");
        return httpd_resp_send(req, NULL, 0);
    }

    if (gui_downloader_files_exist()) {
        char fp[64];
        snprintf(fp, sizeof(fp), "%s/index.html", FILE_MGR_WEB_PATH);
        if (file_manager_exists(fp)) {
            return web_server_send_file(req, fp);
        }
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup.html");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_setup_page(httpd_req_t *req)
{
    ws_request_count++;
    const char *html = web_assets_get_setup_html();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// ============================================================================
// 404 handler (statik dosya servisi)
// ============================================================================

static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    ws_request_count++;

    const char *uri = req->uri;

    if (!config_is_setup_completed()) {
        if (strncmp(uri, "/api/setup/", 11) != 0 &&
            strcmp(uri, "/setup.html") != 0) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/setup.html");
            return httpd_resp_send(req, NULL, 0);
        }
    }

    char filepath[576];
    snprintf(filepath, sizeof(filepath), "%s%s", FILE_MGR_WEB_PATH, uri);

    if (strstr(filepath, "..")) {
        return web_server_send_error(req, 403, "Forbidden");
    }

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

#define REG(path, meth, hdlr) do { \
    httpd_uri_t u = {.uri = path, .method = meth, .handler = hdlr}; \
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
    REG("/setup.html", HTTP_GET, h_setup_page);

    // Auth API
    REG("/api/login", HTTP_POST, h_api_login);
    REG("/api/logout", HTTP_POST, h_api_logout);

    // Timer API
    REG("/api/timer/status", HTTP_GET, h_api_timer_status);
    REG("/api/timer/reset", HTTP_POST, h_api_timer_reset);
    REG("/api/config/timer", HTTP_GET, h_api_config_timer_get);
    REG("/api/config/timer", HTTP_POST, h_api_config_timer_post);
    REG("/api/timer/enable", HTTP_POST, h_api_timer_enable);
    REG("/api/timer/disable", HTTP_POST, h_api_timer_disable);
    REG("/api/timer/acknowledge", HTTP_POST, h_api_timer_acknowledge);
    REG("/api/timer/vacation", HTTP_POST, h_api_timer_vacation);

    // Mail API
    REG("/api/config/mail", HTTP_GET, h_api_config_mail_get);
    REG("/api/config/mail", HTTP_POST, h_api_config_mail_post);
    REG("/api/mail/test", HTTP_POST, h_api_mail_test);
    REG("/api/mail/stats", HTTP_GET, h_api_mail_stats);
    REG("/api/config/smtp", HTTP_GET, h_api_config_smtp_get);
    REG("/api/config/smtp", HTTP_POST, h_api_config_smtp_post);
    REG("/api/test/smtp", HTTP_POST, h_api_test_smtp);
    REG("/api/config/mail-groups", HTTP_GET, h_api_config_mail_groups_get);
    REG("/api/config/mail-groups", HTTP_POST, h_api_config_mail_groups_post);

    // Device API
    REG("/api/device/info", HTTP_GET, h_api_device_info);
    REG("/api/status", HTTP_GET, h_api_status);
    REG("/api/reboot", HTTP_POST, h_api_reboot);
    REG("/api/factory-reset", HTTP_POST, h_api_factory_reset);

    // Relay API
    REG("/api/relay/status", HTTP_GET, h_api_relay_status);
    REG("/api/relay/control", HTTP_POST, h_api_relay_control);
    REG("/api/relay/test", HTTP_POST, h_api_relay_test);
    REG("/api/config/relay", HTTP_GET, h_api_config_relay_get);
    REG("/api/config/relay", HTTP_POST, h_api_config_relay_post);

    // WiFi API
    REG("/api/wifi/status", HTTP_GET, h_api_wifi_status);
    REG("/api/config/wifi", HTTP_GET, h_api_config_wifi_get);
    REG("/api/config/wifi", HTTP_POST, h_api_config_wifi_post);
    REG("/api/config/ap", HTTP_POST, h_api_config_ap);

    // Setup API
    REG("/api/setup/status", HTTP_GET, h_api_setup_status);
    REG("/api/setup/wifi/scan", HTTP_GET, h_api_setup_wifi_scan);
    REG("/api/setup/wifi/connect", HTTP_POST, h_api_setup_wifi_connect);
    REG("/api/setup/password", HTTP_POST, h_api_setup_password);
    REG("/api/setup/complete", HTTP_POST, h_api_setup_complete);
    REG("/api/config/password", HTTP_POST, h_api_password_change);
    REG("/api/gui/download", HTTP_POST, h_api_gui_download);
    REG("/api/gui/download/status", HTTP_GET, h_api_gui_download_status);

    // OTA API
    REG("/api/ota/status", HTTP_GET, h_api_ota_status);
    REG("/api/ota/url", HTTP_POST, h_api_ota_url);
    REG("/api/ota/check", HTTP_GET, h_api_ota_check);

    // Config API
    REG("/api/config/security", HTTP_GET, h_api_config_security_get);
    REG("/api/config/security", HTTP_POST, h_api_config_security_post);
    REG("/api/config/security/api-key", HTTP_POST, h_api_config_security_apikey);
    REG("/api/config/export", HTTP_GET, h_api_config_export);
    REG("/api/config/export", HTTP_POST, h_api_config_export);
    REG("/api/config/import", HTTP_POST, h_api_config_import);
    REG("/api/config/webhook", HTTP_POST, h_api_config_webhook);
    REG("/api/config/telegram", HTTP_POST, h_api_config_telegram);
    REG("/api/config/early-mail", HTTP_POST, h_api_config_early_mail);

    // Logs API
    REG("/api/logs", HTTP_GET, h_api_logs_get);
    REG("/api/logs", HTTP_DELETE, h_api_logs_delete);

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
    ESP_LOGI(TAG, "│ Istek:     %lu", ws_request_count);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
