/**
 * LebensSpur - Web Server
 * Dahili flash: API endpointleri (ayarlar)
 * Harici flash: Statik dosyalar (HTML/CSS/JS/JSON)
 * Harici flash erisilemezse: Gomulu (embedded) dosyalardan sun
 */

#include "web_server.h"
#include "ext_flash.h"
#include "wifi_manager.h"
#include "timer_manager.h"
#include "smtp_client.h"
#include "system_info.h"
#include "device_log.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_random.h"
#include "output.h"
#include "telegram_client.h"
#include "gui_ota.h"
#include "fw_ota.h"
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "WEB_SRV";
static httpd_handle_t s_server = NULL;

// SSE bagli istemciler (max 3 eszamanli baglanti)
#define SSE_MAX_CLIENTS 3
static int s_sse_fds[SSE_MAX_CLIENTS] = {-1, -1, -1};

// SSE gonderim task'i icin flag
static volatile bool s_sse_pending = false;
static TaskHandle_t s_sse_task_handle = NULL;

// ============================================================
// Gomulu (embedded) GUI dosyalari - firmware icinden
// ============================================================
extern const char gui_index_html_start[] asm("_binary_index_html_start");
extern const char gui_index_html_end[]   asm("_binary_index_html_end");
extern const char gui_style_css_start[]  asm("_binary_style_css_start");
extern const char gui_style_css_end[]    asm("_binary_style_css_end");
extern const char gui_app_js_start[]     asm("_binary_app_js_start");
extern const char gui_app_js_end[]       asm("_binary_app_js_end");
extern const char gui_config_json_start[] asm("_binary_config_json_start");
extern const char gui_config_json_end[]   asm("_binary_config_json_end");
extern const char gui_logo_png_start[]      asm("_binary_logo_png_start");
extern const char gui_logo_png_end[]        asm("_binary_logo_png_end");
extern const char gui_darklogo_png_start[]  asm("_binary_darklogo_png_start");
extern const char gui_darklogo_png_end[]    asm("_binary_darklogo_png_end");
extern const char gui_lang_en_json_start[]  asm("_binary_en_json_start");
extern const char gui_lang_en_json_end[]    asm("_binary_en_json_end");
extern const char gui_lang_de_json_start[]  asm("_binary_de_json_start");
extern const char gui_lang_de_json_end[]    asm("_binary_de_json_end");
extern const char gui_lang_es_json_start[]  asm("_binary_es_json_start");
extern const char gui_lang_es_json_end[]    asm("_binary_es_json_end");
extern const char gui_lang_fr_json_start[]  asm("_binary_fr_json_start");
extern const char gui_lang_fr_json_end[]    asm("_binary_fr_json_end");
extern const char gui_lang_it_json_start[]  asm("_binary_it_json_start");
extern const char gui_lang_it_json_end[]    asm("_binary_it_json_end");
extern const char gui_lang_tr_json_start[]  asm("_binary_tr_json_start");
extern const char gui_lang_tr_json_end[]    asm("_binary_tr_json_end");

// Gomulu dosya arama tablosu
typedef struct {
    const char *path;
    const char *start;
    const char *end;
    const char *content_type;
} embedded_file_t;

static const embedded_file_t s_embedded_files[] = {
    { "/index.html",   NULL, NULL, "text/html" },
    { "/style.css",    NULL, NULL, "text/css" },
    { "/app.js",       NULL, NULL, "application/javascript" },
    { "/config.json",  NULL, NULL, "application/json" },
    { "/pic/logo.png",     NULL, NULL, "image/png" },
    { "/pic/darklogo.png", NULL, NULL, "image/png" },
    { "/lang/en.json",    NULL, NULL, "application/json" },
    { "/lang/de.json",    NULL, NULL, "application/json" },
    { "/lang/es.json",    NULL, NULL, "application/json" },
    { "/lang/fr.json",    NULL, NULL, "application/json" },
    { "/lang/it.json",    NULL, NULL, "application/json" },
    { "/lang/tr.json",    NULL, NULL, "application/json" },
};
#define EMBEDDED_FILES_COUNT (sizeof(s_embedded_files) / sizeof(s_embedded_files[0]))

// Gomulu dosya bul - runtime init yapilmasi gerekiyor
static const char *get_embedded_start(int idx) {
    switch (idx) {
        case 0: return gui_index_html_start;
        case 1: return gui_style_css_start;
        case 2: return gui_app_js_start;
        case 3: return gui_config_json_start;
        case 4: return gui_logo_png_start;
        case 5: return gui_darklogo_png_start;
        case 6: return gui_lang_en_json_start;
        case 7: return gui_lang_de_json_start;
        case 8: return gui_lang_es_json_start;
        case 9: return gui_lang_fr_json_start;
        case 10: return gui_lang_it_json_start;
        case 11: return gui_lang_tr_json_start;
        default: return NULL;
    }
}

static const char *get_embedded_end(int idx) {
    switch (idx) {
        case 0: return gui_index_html_end;
        case 1: return gui_style_css_end;
        case 2: return gui_app_js_end;
        case 3: return gui_config_json_end;
        case 4: return gui_logo_png_end;
        case 5: return gui_darklogo_png_end;
        case 6: return gui_lang_en_json_end;
        case 7: return gui_lang_de_json_end;
        case 8: return gui_lang_es_json_end;
        case 9: return gui_lang_fr_json_end;
        case 10: return gui_lang_it_json_end;
        case 11: return gui_lang_tr_json_end;
        default: return NULL;
    }
}

// Basit JSON string parse helper
static int parse_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    char *start = strstr(json, search);
    if (!start) return -1;
    start += strlen(search);
    char *end = strchr(start, '"');
    if (!end) return -1;
    size_t len = end - start;
    if (len >= out_size) len = out_size - 1;
    strncpy(out, start, len);
    out[len] = '\0';
    return (int)len;
}

/* JSON string icindeki ozel karakterleri escape'le (" \ \n \r \t) */
static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t si = 0, di = 0;
    while (src[si] && di < dst_size - 1) {
        char c = src[si++];
        const char *esc = NULL;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default: break;
        }
        if (esc) {
            size_t el = strlen(esc);
            if (di + el > dst_size - 1) break;
            memcpy(&dst[di], esc, el);
            di += el;
        } else if ((unsigned char)c >= 0x20) {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return di;
}

// ============================================================
// HTTP Handler'lar
// ============================================================

// Chunked dosya gonderim boyutu
#define FILE_CHUNK_SIZE 4096

// Aktif slot'tan dosyayi chunked olarak gonder (tek fopen/fclose)
static esp_err_t send_active_slot_file_chunked(httpd_req_t *req, const char *path)
{
    long file_size = 0;
    ext_flash_file_t f = ext_flash_active_fopen(path, &file_size);
    if (!f || file_size <= 0) return ESP_FAIL;

    char *chunk = malloc(FILE_CHUNK_SIZE);
    if (!chunk) {
        ext_flash_active_fclose(f);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%s aktif slot'tan sunuluyor (%ld byte, chunked)", path, file_size);

    esp_err_t result = ESP_OK;
    int read_len;
    while ((read_len = ext_flash_active_fread(f, chunk, FILE_CHUNK_SIZE)) > 0) {
        esp_err_t err = httpd_resp_send_chunk(req, chunk, read_len);
        if (err != ESP_OK) {
            result = err;
            break;
        }
    }

    free(chunk);
    ext_flash_active_fclose(f);

    if (result == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0); // Son chunk (bitis)
    }
    return result;
}

// GET / - Ana sayfa (aktif slot veya gomulu)
static esp_err_t root_handler(httpd_req_t *req)
{
    // Once aktif slot'tan dene (GUI web arayuzu)
    if (ext_flash_is_slot_a_mounted() || ext_flash_is_slot_b_mounted()) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        if (send_active_slot_file_chunked(req, "/index.html") == ESP_OK) {
            return ESP_OK;
        }
    }

    // Gomulu index.html sun
    size_t len = gui_index_html_end - gui_index_html_start;
    ESP_LOGI(TAG, "index.html gomulu veriden sunuluyor (%zu byte)", len);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, gui_index_html_start, len);
}

// GET /api/system/info - Sistem bilgisi (cihaz adi, AP, STA, mDNS, flash)
static esp_err_t api_system_info_handler(httpd_req_t *req)
{
    char resp[640];
    size_t slot_a_total = 0, slot_a_free = 0;
    size_t user_total = 0, user_free = 0;

    if (ext_flash_is_slot_a_mounted()) {
        ext_flash_get_slot_a_info(&slot_a_total, &slot_a_free);
    }
    if (ext_flash_is_user_data_mounted()) {
        ext_flash_get_info(&user_total, &user_free);
    }

    snprintf(resp, sizeof(resp),
             "{"
             "\"device_name\":\"%s\","
             "\"ip\":\"%s\","
             "\"ap_ssid\":\"%s\","
             "\"ap_ip\":\"192.168.4.1\","
             "\"sta_ssid\":\"%s\","
             "\"connected\":%s,"
             "\"mdns\":\"%s.local\","
             "\"ext_flash\":%s,"
             "\"slot_a_total\":%zu,"
             "\"slot_a_free\":%zu,"
             "\"user_data_total\":%zu,"
             "\"user_data_free\":%zu"
             "}",
             wifi_manager_get_device_name(),
             wifi_manager_get_ip(),
             wifi_manager_get_ap_ssid(),
             wifi_manager_get_sta_ssid(),
             wifi_manager_is_connected() ? "true" : "false",
             wifi_manager_get_device_name(),
             ext_flash_is_mounted() ? "true" : "false",
             slot_a_total / 1024,
             slot_a_free / 1024,
             user_total / 1024,
             user_free / 1024);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/wifi/connect - WiFi baglanti (ssid, password, static_ip)
static esp_err_t api_wifi_connect_handler(httpd_req_t *req)
{
    char buf[384];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    char static_ip[16] = {0};

    parse_json_string(buf, "ssid", ssid, sizeof(ssid));
    parse_json_string(buf, "password", password, sizeof(password));
    parse_json_string(buf, "static_ip", static_ip, sizeof(static_ip));

    ESP_LOGI("WEB_SRV", "WiFi connect - SSID:'%s' pass_len:%d ip:'%s'",
             ssid, (int)strlen(password), static_ip);

    if (strlen(ssid) == 0) {
        wifi_manager_clear_primary();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"WiFi ayarlari silindi\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = wifi_manager_connect_sta(ssid, password, static_ip);
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"message\":\"Baglaniliyor: %s\"}", ssid);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"Hata: %s\"}", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/wifi/backup/save - Yedek WiFi kaydet
static esp_err_t api_wifi_backup_save_handler(httpd_req_t *req)
{
    char buf[384];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    char static_ip[16] = {0};

    parse_json_string(buf, "ssid", ssid, sizeof(ssid));
    parse_json_string(buf, "password", password, sizeof(password));
    parse_json_string(buf, "static_ip", static_ip, sizeof(static_ip));

    ESP_LOGI("WEB_SRV", "Backup WiFi save - SSID:'%s' pass_len:%d ip:'%s'",
             ssid, (int)strlen(password), static_ip);

    if (strlen(ssid) == 0) {
        wifi_manager_clear_backup();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Yedek WiFi ayarlari silindi\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = wifi_manager_save_backup(ssid, password, static_ip);
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"message\":\"Yedek WiFi kaydedildi: %s\"}", ssid);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"Hata: %s\"}", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/wifi/config - WiFi ayarlarini getir (primary + backup)
static esp_err_t api_wifi_config_handler(httpd_req_t *req)
{
    char resp[512];
    snprintf(resp, sizeof(resp),
             "{"
             "\"connected\":%s,"
             "\"ip\":\"%s\","
             "\"sta_ssid\":\"%s\","
             "\"backup_ssid\":\"%s\","
             "\"has_backup\":%s,"
             "\"ap_enabled\":%s"
             "}",
             wifi_manager_is_connected() ? "true" : "false",
             wifi_manager_get_ip(),
             wifi_manager_get_sta_ssid(),
             wifi_manager_get_backup_ssid(),
             wifi_manager_has_backup_config() ? "true" : "false",
             wifi_manager_is_ap_enabled() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/wifi/ap/set - AP modunu ac/kapat
static esp_err_t api_wifi_ap_set_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // "enabled" degerini parse et (true/false veya 1/0)
    char enabled_str[8] = {0};
    parse_json_string(buf, "enabled", enabled_str, sizeof(enabled_str));
    bool enabled = (strcmp(enabled_str, "true") == 0 || strcmp(enabled_str, "1") == 0);

    esp_err_t err = wifi_manager_set_ap_enabled(enabled);
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"ap_enabled\":%s}",
                 wifi_manager_is_ap_enabled() ? "true" : "false");
    } else if (err == ESP_ERR_INVALID_STATE) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"WiFi bagli degil, AP kapatilamaz\"}");
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"%s\"}", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/timer/get - Zamanlayici durumunu getir (ana + tatil + alarm)
static esp_err_t api_timer_get_handler(httpd_req_t *req)
{
    timer_state_t state = timer_manager_get_state();

    char resp[384];
    snprintf(resp, sizeof(resp),
             "{\"enabled\":%s,\"remaining_seconds\":%ld,\"total_seconds\":%ld,"
             "\"vacation_enabled\":%s,\"vacation_remaining\":%ld,\"vacation_total\":%ld,"
             "\"alarm_count\":%d}",
             state.enabled ? "true" : "false",
             (long)state.remaining_seconds,
             (long)state.total_seconds,
             state.vacation_enabled ? "true" : "false",
             (long)state.vacation_remaining,
             (long)state.vacation_total,
             state.alarm_count);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/timer/set - Zamanlayici ayarla veya iptal et
static esp_err_t api_timer_set_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Aksiyon kontrolu
    char action[20] = {0};
    parse_json_string(buf, "action", action, sizeof(action));

    if (strcmp(action, "cancel") == 0) {
        timer_manager_cancel();
        web_server_send_timer_event();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\",\"remaining_seconds\":0}", HTTPD_RESP_USE_STRLEN);
    }
    if (strcmp(action, "restart") == 0) {
        esp_err_t err = timer_manager_restart();
        if (err == ESP_OK) {
            web_server_send_timer_event();
            timer_state_t st = timer_manager_get_state();
            char rsp[128];
            snprintf(rsp, sizeof(rsp),
                     "{\"status\":\"ok\",\"remaining_seconds\":%ld,\"total_seconds\":%ld}",
                     (long)st.remaining_seconds, (long)st.total_seconds);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, rsp, strlen(rsp));
        }
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Onceden ayarlanmis sure yok\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Tatil modu baslat
    if (strcmp(action, "vacation_set") == 0) {
        char days_str[8] = {0};
        parse_json_string(buf, "days", days_str, sizeof(days_str));
        int days = atoi(days_str);
        esp_err_t err = timer_manager_set_vacation(days);
        if (err == ESP_OK) {
            web_server_send_timer_event();
            timer_state_t st = timer_manager_get_state();
            char rsp[256];
            snprintf(rsp, sizeof(rsp),
                     "{\"status\":\"ok\",\"vacation_remaining\":%ld,\"vacation_total\":%ld}",
                     (long)st.vacation_remaining, (long)st.vacation_total);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, rsp, strlen(rsp));
        }
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Tatil ayarlanamadi\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Tatil modu iptal
    if (strcmp(action, "vacation_cancel") == 0) {
        timer_manager_cancel_vacation();
        web_server_send_timer_event();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Alarm sayisi ayarla
    if (strcmp(action, "set_alarms") == 0) {
        char count_str[8] = {0};
        parse_json_string(buf, "count", count_str, sizeof(count_str));
        int count = atoi(count_str);
        esp_err_t err = timer_manager_set_alarm_count(count);
        if (err == ESP_OK) {
            web_server_send_timer_event();
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
        }
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Gecersiz alarm sayisi\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Normal timer ayarla
    char unit[8] = {0};
    char value_str[8] = {0};

    parse_json_string(buf, "unit", unit, sizeof(unit));
    parse_json_string(buf, "value", value_str, sizeof(value_str));

    int value = atoi(value_str);
    if (value < 1 || value > 60) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Deger 1-60 arasi olmali");
        return ESP_FAIL;
    }

    esp_err_t err = timer_manager_set(unit, value);
    char resp[192];
    if (err == ESP_OK) {
        web_server_send_timer_event();
        timer_state_t state = timer_manager_get_state();
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"remaining_seconds\":%ld,\"total_seconds\":%ld}",
                 (long)state.remaining_seconds, (long)state.total_seconds);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"Timer ayarlanamadi\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/smtp/get - SMTP ayarlarini getir
static esp_err_t api_smtp_get_handler(httpd_req_t *req)
{
    smtp_config_t cfg;
    smtp_client_get_config(&cfg);
    bool has = smtp_client_has_config();

    char resp[384];
    snprintf(resp, sizeof(resp),
             "{\"configured\":%s,\"server\":\"%s\",\"port\":%d,\"user\":\"%s\"}",
             has ? "true" : "false",
             cfg.server, cfg.port, cfg.user);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/smtp/save - SMTP ayarlarini kaydet
static esp_err_t api_smtp_save_handler(httpd_req_t *req)
{
    char buf[384];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    smtp_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    parse_json_string(buf, "server", cfg.server, sizeof(cfg.server));

    char port_str[8] = {0};
    parse_json_string(buf, "port", port_str, sizeof(port_str));
    cfg.port = (uint16_t)atoi(port_str);
    if (cfg.port == 0) cfg.port = 465;

    parse_json_string(buf, "user", cfg.user, sizeof(cfg.user));
    parse_json_string(buf, "api_key", cfg.api_key, sizeof(cfg.api_key));

    // api_key bos geldiyse mevcut NVS degerini koru (sifre sayfaya yuklenmez)
    if (strlen(cfg.api_key) == 0) {
        smtp_config_t existing;
        if (smtp_client_get_config(&existing) == ESP_OK && strlen(existing.api_key) > 0) {
            strncpy(cfg.api_key, existing.api_key, sizeof(cfg.api_key) - 1);
        }
    }

    if (strlen(cfg.server) == 0 || strlen(cfg.user) == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Server ve email gerekli\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = smtp_client_save_config(&cfg);
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"message\":\"SMTP ayarlari kaydedildi\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Kaydetme hatasi\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/smtp/test - Test maili gonder
static esp_err_t api_smtp_test_handler(httpd_req_t *req)
{
    if (!smtp_client_has_config()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"SMTP yapilandirilmamis\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = smtp_client_send_test();
    char resp[256];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"message\":\"Test maili gonderildi\"}");
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Auth hatasi - email/sifre yanlis\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Baglanti hatasi - sunucu/port kontrol edin (%s)\"}", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/system/detail - Detayli sistem bilgisi
static esp_err_t api_system_detail_handler(httpd_req_t *req)
{
    system_info_t info;
    system_info_get(&info);

    size_t slot_a_total = 0, slot_a_free = 0;
    size_t user_total = 0, user_free = 0;
    if (ext_flash_is_slot_a_mounted()) {
        ext_flash_get_slot_a_info(&slot_a_total, &slot_a_free);
    }
    if (ext_flash_is_user_data_mounted()) {
        ext_flash_get_info(&user_total, &user_free);
    }

    char resp[768];
    snprintf(resp, sizeof(resp),
             "{"
             "\"chip\":\"%s\","
             "\"cores\":%u,"
             "\"cpu_mhz\":%lu,"
             "\"flash_kb\":%lu,"
             "\"flash_used_kb\":%lu,"
             "\"heap_total\":%lu,"
             "\"heap_free\":%lu,"
             "\"heap_min\":%lu,"
             "\"uptime\":%lld,"
             "\"rssi\":%d,"
             "\"reset_reason\":\"%s\","
             "\"fw_version\":\"%s\","
             "\"mdns\":\"%s.local\","
             "\"slot_a_total\":%zu,"
             "\"slot_a_free\":%zu,"
             "\"slot_b_kb\":2048,"
             "\"user_total\":%zu,"
             "\"user_free\":%zu"
             "}",
             info.chip_model,
             (unsigned)info.cores,
             (unsigned long)info.cpu_freq_mhz,
             (unsigned long)info.flash_size_kb,
             (unsigned long)info.flash_used_kb,
             (unsigned long)info.heap_total,
             (unsigned long)info.heap_free,
             (unsigned long)info.heap_min_free,
             (long long)info.uptime_seconds,
             info.wifi_rssi,
             info.reset_reason,
             info.fw_version,
             wifi_manager_get_device_name(),
             slot_a_total / 1024,
             slot_a_free / 1024,
             user_total / 1024,
             user_free / 1024);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/device/restart - Cihazi yeniden baslat
static esp_err_t api_device_restart_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Yeniden baslatma istegi alindi (web)");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// POST /api/device/factory-reset - Fabrika ayarlarina don
static esp_err_t api_device_factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory Reset istegi alindi (web)");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);

    // NVS sil (WiFi, SMTP, timer, output ayarlari)
    nvs_flash_erase();

    // Harici flash kullanici verilerini temizle
    ext_flash_format_user_data();

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// GET /api/relay/get - Role ayarlarini getir
static esp_err_t api_relay_get_handler(httpd_req_t *req)
{
    uint16_t duration = 0;
    uint8_t pulse = 0, delay_v = 0;
    output_get_relay_config(&duration, &pulse, &delay_v);
    bool inverted = output_get_inverted();
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"duration\":%d,\"pulse\":%d,\"delay\":%d,\"inverted\":%s}",
             duration, pulse, delay_v, inverted ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/relay/set - Role ayarlarini kaydet
static esp_err_t api_relay_set_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // "duration" parse et (toplam calisma suresi, saniye)
    char *p = strstr(buf, "\"duration\"");
    uint16_t duration = 0;
    if (p) {
        p = strchr(p, ':');
        if (p) duration = (uint16_t)atoi(p + 1);
    }

    // "pulse" parse et (pulse suresi, saniye)
    p = strstr(buf, "\"pulse\"");
    uint8_t pulse = 0;
    if (p) {
        p = strchr(p, ':');
        if (p) pulse = (uint8_t)atoi(p + 1);
    }

    // "delay" parse et (baslanma gecikmesi, saniye)
    p = strstr(buf, "\"delay\"");
    uint8_t delay_v = 0;
    if (p) {
        p = strchr(p, ':');
        if (p) delay_v = (uint8_t)atoi(p + 1);
    }

    // "inverted" parse et
    char inv_str[8] = {0};
    if (parse_json_string(buf, "inverted", inv_str, sizeof(inv_str)) >= 0) {
        output_set_inverted(strcmp(inv_str, "true") == 0);
    }

    output_set_relay_config(duration, pulse, delay_v);
    output_save_config();
    ESP_LOGI(TAG, "Role ayarlari kaydedildi: dur=%d pulse=%d delay=%d inv=%d",
             duration, pulse, delay_v, (int)output_get_inverted());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// POST /api/relay/test - Roleyi test et (kisa pulse)
static esp_err_t api_relay_test_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Role test tetiklendi (web)");
    output_relay_trigger();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/logs - Son log kayitlari
static esp_err_t api_logs_handler(httpd_req_t *req)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bellek yetersiz");
        return ESP_FAIL;
    }

    device_log_to_json(buf, 4096, 0);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, buf, strlen(buf));
    free(buf);
    return ret;
}

// ============================================================
// SSE (Server-Sent Events) - Anlik timer durumu
// ============================================================

// GET /api/events - SSE stream
static esp_err_t api_events_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return ESP_FAIL;

    // Bos slot bul
    int slot = -1;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_sse_fds[i] < 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // Yer yok, en eski baglantiyi kapat
        httpd_sess_trigger_close(req->handle, s_sse_fds[0]);
        s_sse_fds[0] = -1;
        slot = 0;
    }

    s_sse_fds[slot] = fd;

    // SSE header'lari gonder
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Bos yanit gonder (header'lari flush etmek icin)
    httpd_resp_send_chunk(req, "data: connected\n\n", 17);

    ESP_LOGI(TAG, "SSE istemci baglandi (fd=%d, slot=%d)", fd, slot);
    return ESP_OK;
}

// SSE gonderim - httpd task context'inde calistirilmali
static void sse_do_send(void)
{
    if (!s_server) return;

    timer_state_t state = timer_manager_get_state();
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "data: {\"enabled\":%s,\"remaining_seconds\":%ld,\"total_seconds\":%ld,"
        "\"vacation_enabled\":%s,\"vacation_remaining\":%ld,\"vacation_total\":%ld,"
        "\"alarm_count\":%d}\n\n",
        state.enabled ? "true" : "false",
        (long)state.remaining_seconds,
        (long)state.total_seconds,
        state.vacation_enabled ? "true" : "false",
        (long)state.vacation_remaining,
        (long)state.vacation_total,
        state.alarm_count);

    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_sse_fds[i] >= 0) {
            int ret = httpd_socket_send(s_server, s_sse_fds[i], buf, len, 0);
            if (ret < 0) {
                ESP_LOGW(TAG, "SSE gonderim basarisiz (fd=%d), kapatiliyor", s_sse_fds[i]);
                httpd_sess_trigger_close(s_server, s_sse_fds[i]);
                s_sse_fds[i] = -1;
            }
        }
    }
}

// SSE gonderim task'i - ayrı FreeRTOS task, timer callback'ten guvenlice tetiklenir
static void sse_send_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_sse_pending = false;
        sse_do_send();
    }
}

// Tum bagli SSE istemcilere timer durumunu gonder
// esp_timer callback'ten veya herhangi bir context'ten guvenle cagrilabilir
void web_server_send_timer_event(void)
{
    if (!s_server || !s_sse_task_handle) return;
    s_sse_pending = true;
    xTaskNotifyGive(s_sse_task_handle);
}

// GET /ext/* - Statik dosya sun (Slot A veya gomulu fallback)
static esp_err_t ext_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    // /ext/ prefix'ini atla -> dosya yolu
    const char *file_path = uri + 4;
    if (strlen(file_path) == 0 || strcmp(file_path, "/") == 0) {
        file_path = "/index.html";
    }

    // Query string'i (?v=0.0.40 gibi) dosya yolundan ayir
    char clean_path[128];
    strncpy(clean_path, file_path, sizeof(clean_path) - 1);
    clean_path[sizeof(clean_path) - 1] = '\0';
    char *qmark = strchr(clean_path, '?');
    if (qmark) *qmark = '\0';

    // Once aktif slot'tan dene (GUI web arayuzu)
    if (ext_flash_is_slot_a_mounted() || ext_flash_is_slot_b_mounted()) {
        long fsize = ext_flash_active_get_file_size(clean_path);
        if (fsize > 0) {
            // Content-Type + Cache-Control belirle
            const char *ext = strrchr(clean_path, '.');
            bool cacheable = false;
            if (ext) {
                if (strcmp(ext, ".html") == 0) httpd_resp_set_type(req, "text/html");
                else if (strcmp(ext, ".css") == 0)  { httpd_resp_set_type(req, "text/css"); cacheable = true; }
                else if (strcmp(ext, ".js") == 0)   { httpd_resp_set_type(req, "application/javascript"); cacheable = true; }
                else if (strcmp(ext, ".json") == 0) httpd_resp_set_type(req, "application/json");
                else if (strcmp(ext, ".png") == 0)  { httpd_resp_set_type(req, "image/png"); cacheable = true; }
                else if (strcmp(ext, ".ico") == 0)  { httpd_resp_set_type(req, "image/x-icon"); cacheable = true; }
                else httpd_resp_set_type(req, "text/plain");
            }
            if (cacheable) {
                httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
            } else {
                httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            }
            return send_active_slot_file_chunked(req, clean_path);
        }
    }

    // Harici flash'ta bulunamadi - gomulu dosyalardan ara
    for (int i = 0; i < (int)EMBEDDED_FILES_COUNT; i++) {
        if (strcmp(clean_path, s_embedded_files[i].path) == 0) {
            const char *start = get_embedded_start(i);
            const char *end = get_embedded_end(i);
            size_t len = end - start;
            ESP_LOGI(TAG, "%s gomulu veriden sunuluyor (%zu byte)", file_path, len);
            httpd_resp_set_type(req, s_embedded_files[i].content_type);
            return httpd_resp_send(req, start, len);
        }
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Dosya bulunamadi");
    return ESP_FAIL;
}

// ============================================================
// Erken Uyari Mail API (NVS namespace: ls_ew_mail)
// ============================================================

// GET /api/early-mail/get
static esp_err_t api_early_mail_get_handler(httpd_req_t *req)
{
    nvs_handle_t nvs;
    char email[64] = {0}, subject[64] = {0}, body[256] = {0};
    bool configured = false;

    if (nvs_open("ls_ew_mail", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(email);
        if (nvs_get_str(nvs, "email", email, &len) == ESP_OK && strlen(email) > 0) {
            configured = true;
        }
        len = sizeof(subject);
        nvs_get_str(nvs, "subject", subject, &len);
        len = sizeof(body);
        nvs_get_str(nvs, "body", body, &len);
        nvs_close(nvs);
    }

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"configured\":%s,\"email\":\"%s\",\"subject\":\"%s\",\"body\":\"%s\"}",
             configured ? "true" : "false", email, subject, body);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/early-mail/save
static esp_err_t api_early_mail_save_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char email[64] = {0}, subject[64] = {0}, body[256] = {0};
    parse_json_string(buf, "email", email, sizeof(email));
    parse_json_string(buf, "subject", subject, sizeof(subject));
    parse_json_string(buf, "body", body, sizeof(body));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ls_ew_mail", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"NVS hatasi\"}", HTTPD_RESP_USE_STRLEN);
    }

    nvs_set_str(nvs, "email", email);
    nvs_set_str(nvs, "subject", subject);
    nvs_set_str(nvs, "body", body);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Erken uyari mail kaydedildi: %s", email);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// ============================================================
// Tetikleme Mail Grup API (NVS namespace: ls_tg_mail)
// ============================================================

// GET /api/trigger-mail/get
static esp_err_t api_trigger_mail_get_handler(httpd_req_t *req)
{
    nvs_handle_t nvs;
    uint8_t count = 0;

    // Yeterli tampon - her grup ~300 byte, max 10 grup
    char *resp = malloc(4096);
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bellek yetersiz");
        return ESP_FAIL;
    }

    int pos = 0;
    pos += snprintf(resp + pos, 4096 - pos, "{\"count\":");

    if (nvs_open("ls_tg_mail", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "count", &count);
        pos += snprintf(resp + pos, 4096 - pos, "%d,\"groups\":[", count);

        for (int i = 0; i < count && i < 10; i++) {
            char key[20];
            char name[64] = {0}, subj[64] = {0}, body[256] = {0}, rcpt[256] = {0};
            size_t len;

            snprintf(key, sizeof(key), "g%d_name", i);
            len = sizeof(name); nvs_get_str(nvs, key, name, &len);

            snprintf(key, sizeof(key), "g%d_subj", i);
            len = sizeof(subj); nvs_get_str(nvs, key, subj, &len);

            snprintf(key, sizeof(key), "g%d_body", i);
            len = sizeof(body); nvs_get_str(nvs, key, body, &len);

            snprintf(key, sizeof(key), "g%d_rcpt", i);
            len = sizeof(rcpt); nvs_get_str(nvs, key, rcpt, &len);

            if (i > 0) pos += snprintf(resp + pos, 4096 - pos, ",");
            pos += snprintf(resp + pos, 4096 - pos,
                "{\"id\":%d,\"name\":\"%s\",\"subject\":\"%s\",\"body\":\"%s\",\"recipients\":\"%s\"}",
                i, name, subj, body, rcpt);
        }
        nvs_close(nvs);
    } else {
        pos += snprintf(resp + pos, 4096 - pos, "0,\"groups\":[");
    }

    pos += snprintf(resp + pos, 4096 - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, resp, strlen(resp));
    free(resp);
    return r;
}

// POST /api/trigger-mail/save
static esp_err_t api_trigger_mail_save_handler(httpd_req_t *req)
{
    char buf[768];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // id parse (-1 = yeni, >=0 = guncelle)
    char id_str[8] = {0};
    parse_json_string(buf, "id", id_str, sizeof(id_str));
    int id = atoi(id_str);
    // atoi("") = 0, bu yuzden "-1" ozel anlam tasir
    if (strlen(id_str) == 0) id = -1;

    char name[64] = {0}, subj[64] = {0}, body[256] = {0}, rcpt[256] = {0};
    parse_json_string(buf, "name", name, sizeof(name));
    parse_json_string(buf, "subject", subj, sizeof(subj));
    parse_json_string(buf, "body", body, sizeof(body));
    parse_json_string(buf, "recipients", rcpt, sizeof(rcpt));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ls_tg_mail", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"NVS hatasi\"}", HTTPD_RESP_USE_STRLEN);
    }

    uint8_t count = 0;
    nvs_get_u8(nvs, "count", &count);

    if (id == -1) {
        // Yeni grup olustur
        if (count >= 10) {
            nvs_close(nvs);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Maksimum 10 grup\"}", HTTPD_RESP_USE_STRLEN);
        }
        id = count;
        count++;
        nvs_set_u8(nvs, "count", count);
    } else if (id < 0 || id >= count) {
        nvs_close(nvs);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Gecersiz grup ID\"}", HTTPD_RESP_USE_STRLEN);
    }

    char key[20];
    snprintf(key, sizeof(key), "g%d_name", id);
    nvs_set_str(nvs, key, name);
    snprintf(key, sizeof(key), "g%d_subj", id);
    nvs_set_str(nvs, key, subj);
    snprintf(key, sizeof(key), "g%d_body", id);
    nvs_set_str(nvs, key, body);
    snprintf(key, sizeof(key), "g%d_rcpt", id);
    nvs_set_str(nvs, key, rcpt);

    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Tetikleme mail grubu kaydedildi: id=%d name=%s", id, name);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"id\":%d}", id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/trigger-mail/delete
static esp_err_t api_trigger_mail_delete_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char id_str[8] = {0};
    parse_json_string(buf, "id", id_str, sizeof(id_str));
    int id = atoi(id_str);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ls_tg_mail", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"NVS hatasi\"}", HTTPD_RESP_USE_STRLEN);
    }

    uint8_t count = 0;
    nvs_get_u8(nvs, "count", &count);

    if (id < 0 || id >= count) {
        nvs_close(nvs);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Gecersiz grup ID\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Sonraki gruplari kaydir (id+1 -> id, id+2 -> id+1, ...)
    for (int i = id; i < count - 1; i++) {
        char src_key[20], dst_key[20];
        char tmp[256];
        size_t len;

        // name
        snprintf(src_key, sizeof(src_key), "g%d_name", i + 1);
        snprintf(dst_key, sizeof(dst_key), "g%d_name", i);
        len = sizeof(tmp); memset(tmp, 0, sizeof(tmp));
        nvs_get_str(nvs, src_key, tmp, &len);
        nvs_set_str(nvs, dst_key, tmp);

        // subj
        snprintf(src_key, sizeof(src_key), "g%d_subj", i + 1);
        snprintf(dst_key, sizeof(dst_key), "g%d_subj", i);
        len = sizeof(tmp); memset(tmp, 0, sizeof(tmp));
        nvs_get_str(nvs, src_key, tmp, &len);
        nvs_set_str(nvs, dst_key, tmp);

        // body
        snprintf(src_key, sizeof(src_key), "g%d_body", i + 1);
        snprintf(dst_key, sizeof(dst_key), "g%d_body", i);
        len = sizeof(tmp); memset(tmp, 0, sizeof(tmp));
        nvs_get_str(nvs, src_key, tmp, &len);
        nvs_set_str(nvs, dst_key, tmp);

        // rcpt
        snprintf(src_key, sizeof(src_key), "g%d_rcpt", i + 1);
        snprintf(dst_key, sizeof(dst_key), "g%d_rcpt", i);
        len = sizeof(tmp); memset(tmp, 0, sizeof(tmp));
        nvs_get_str(nvs, src_key, tmp, &len);
        nvs_set_str(nvs, dst_key, tmp);
    }

    // Son grubu temizle ve count azalt
    count--;
    nvs_set_u8(nvs, "count", count);

    char key[20];
    snprintf(key, sizeof(key), "g%d_name", (int)count);
    nvs_erase_key(nvs, key);
    snprintf(key, sizeof(key), "g%d_subj", (int)count);
    nvs_erase_key(nvs, key);
    snprintf(key, sizeof(key), "g%d_body", (int)count);
    nvs_erase_key(nvs, key);
    snprintf(key, sizeof(key), "g%d_rcpt", (int)count);
    nvs_erase_key(nvs, key);

    nvs_commit(nvs);
    nvs_close(nvs);

    // Dosya dizinlerini kaydir: silinen grubun dosyalarini temizle, sonrakileri kaydir
    char del_dir[40];
    snprintf(del_dir, sizeof(del_dir), "/ext/mail/g%d", id);
    DIR *dir = opendir(del_dir);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type != DT_REG) continue;
            char fpath[300];
            snprintf(fpath, sizeof(fpath), "%s/%s", del_dir, ent->d_name);
            remove(fpath);
        }
        closedir(dir);
        rmdir(del_dir);
    }
    // Sonraki gruplarin dizinlerini kaydir (g{i+1} -> g{i})
    for (int i = id; i < count; i++) {
        char old_dir[40], new_dir[40];
        snprintf(old_dir, sizeof(old_dir), "/ext/mail/g%d", i + 1);
        snprintf(new_dir, sizeof(new_dir), "/ext/mail/g%d", i);
        rename(old_dir, new_dir);
    }

    ESP_LOGI(TAG, "Tetikleme mail grubu silindi: id=%d, yeni count=%d", id, count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// ============================================================
// Action Card Aktif Durum API (NVS namespace: ls_actions)
// ============================================================

// GET /api/actions/get
static esp_err_t api_actions_get_handler(httpd_req_t *req)
{
    nvs_handle_t nvs;
    uint8_t early_tg = 0, early_mail = 0, early_api = 0;
    uint8_t trig_mail = 0, trig_api = 0, trig_relay = 1;

    if (nvs_open("ls_actions", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "early_tg", &early_tg);
        nvs_get_u8(nvs, "early_mail", &early_mail);
        nvs_get_u8(nvs, "early_api", &early_api);
        nvs_get_u8(nvs, "trig_mail", &trig_mail);
        nvs_get_u8(nvs, "trig_api", &trig_api);
        nvs_get_u8(nvs, "trig_relay", &trig_relay);
        nvs_close(nvs);
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"early_telegram\":%s,\"early_mail\":%s,\"early_api\":%s,"
        "\"trigger_mail\":%s,\"trigger_api\":%s,\"trigger_relay\":%s}",
        early_tg ? "true" : "false",
        early_mail ? "true" : "false",
        early_api ? "true" : "false",
        trig_mail ? "true" : "false",
        trig_api ? "true" : "false",
        trig_relay ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/actions/save
static esp_err_t api_actions_save_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ls_actions", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"NVS hatasi\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Her alan icin: "key":"true" -> 1, yoksa veya "false" -> 0
    const char *keys[] = {"early_telegram", "early_mail", "early_api",
                          "trigger_mail", "trigger_api", "trigger_relay"};
    const char *nvs_keys[] = {"early_tg", "early_mail", "early_api",
                              "trig_mail", "trig_api", "trig_relay"};
    for (int i = 0; i < 6; i++) {
        char val[8] = {0};
        if (parse_json_string(buf, keys[i], val, sizeof(val)) >= 0) {
            nvs_set_u8(nvs, nvs_keys[i], (strcmp(val, "true") == 0) ? 1 : 0);
        }
    }

    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Action durumlar kaydedildi");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// ============================================================
// Telegram API (NVS namespace: ls_telegram)
// ============================================================

// GET /api/telegram/get - Telegram ayarlarini getir (token gizli)
static esp_err_t api_telegram_get_handler(httpd_req_t *req)
{
    telegram_config_t cfg;
    telegram_client_get_config(&cfg);
    bool has = telegram_client_has_config();

    char esc_chat[65];
    char esc_msg[513];
    json_escape(cfg.chat_id, esc_chat, sizeof(esc_chat));
    json_escape(cfg.message, esc_msg, sizeof(esc_msg));

    char resp[640];
    snprintf(resp, sizeof(resp),
             "{\"configured\":%s,\"chat_id\":\"%s\",\"message\":\"%s\"}",
             has ? "true" : "false", esc_chat, esc_msg);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/telegram/save - Telegram ayarlarini kaydet
static esp_err_t api_telegram_save_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    telegram_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    parse_json_string(buf, "token", cfg.token, sizeof(cfg.token));
    parse_json_string(buf, "chat_id", cfg.chat_id, sizeof(cfg.chat_id));
    parse_json_string(buf, "message", cfg.message, sizeof(cfg.message));

    // Token bos geldiyse mevcut NVS degerini koru (sifre sayfaya yuklenmez)
    if (strlen(cfg.token) == 0) {
        telegram_config_t existing;
        if (telegram_client_get_config(&existing) == ESP_OK && strlen(existing.token) > 0) {
            strncpy(cfg.token, existing.token, sizeof(cfg.token) - 1);
        }
    }

    if (strlen(cfg.chat_id) == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Chat ID gerekli\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = telegram_client_save_config(&cfg);
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"message\":\"Telegram ayarlari kaydedildi\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Kaydetme hatasi\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/telegram/test - Test mesaji gonder
static esp_err_t api_telegram_test_handler(httpd_req_t *req)
{
    if (!telegram_client_has_config()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Telegram yapilandirilmamis\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = telegram_client_send("LebensSpur test mesaji basarili!");
    char resp[256];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"message\":\"Test mesaji gonderildi\"}");
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Auth hatasi - token yanlis\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Gonderim hatasi (%s)\"}", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// ============================================================
// Uzaktan Sifirlama API
// ============================================================

// GET /api/reset/config - Reset API yapilandirmasini getir
static esp_err_t api_reset_config_get_handler(httpd_req_t *req)
{
    nvs_handle_t nvs;
    char api_key[16] = {0};
    uint8_t enabled = 0;
    bool has_key = false;
    char masked[16] = {0};

    if (nvs_open("ls_reset", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(api_key);
        if (nvs_get_str(nvs, "api_key", api_key, &len) == ESP_OK && strlen(api_key) > 0) {
            has_key = true;
            // Maskeleme: ls_a3f7****
            size_t kl = strlen(api_key);
            if (kl > 4) {
                memcpy(masked, api_key, kl - 4);
                memcpy(masked + kl - 4, "****", 4);
                masked[kl] = '\0';
            } else {
                strcpy(masked, "****");
            }
        }
        nvs_get_u8(nvs, "enabled", &enabled);
        nvs_close(nvs);
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"enabled\":%s,\"has_key\":%s,\"masked_key\":\"%s\"}",
             enabled ? "true" : "false",
             has_key ? "true" : "false",
             masked);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/reset/config - Reset API durumu kaydet (enabled/disabled)
static esp_err_t api_reset_config_save_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char enabled_str[8] = {0};
    parse_json_string(buf, "enabled", enabled_str, sizeof(enabled_str));
    uint8_t enabled = (strcmp(enabled_str, "true") == 0 || strcmp(enabled_str, "1") == 0) ? 1 : 0;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ls_reset", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, "enabled", enabled);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"enabled\":%s}", enabled ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/reset/generate - Yeni API key uret
static esp_err_t api_reset_generate_handler(httpd_req_t *req)
{
    // ls_ + 8 hex = 11 karakter
    uint32_t rnd = esp_random();
    char api_key[16];
    snprintf(api_key, sizeof(api_key), "ls_%08lx", (unsigned long)rnd);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("ls_reset", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"NVS hatasi\"}", HTTPD_RESP_USE_STRLEN);
    }

    nvs_set_str(nvs, "api_key", api_key);
    // Key olusturulunca otomatik aktif et
    nvs_set_u8(nvs, "enabled", 1);
    nvs_commit(nvs);
    nvs_close(nvs);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"api_key\":\"%s\"}", api_key);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/reset?key=xxx - Timer'i sifirla (dis erisim)
static esp_err_t api_reset_handler(httpd_req_t *req)
{
    // Enabled kontrolu
    nvs_handle_t nvs;
    uint8_t enabled = 0;
    char stored_key[16] = {0};

    if (nvs_open("ls_reset", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "enabled", &enabled);
        size_t len = sizeof(stored_key);
        nvs_get_str(nvs, "api_key", stored_key, &len);
        nvs_close(nvs);
    }

    if (!enabled) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Uzaktan sifirlama devre disi\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Query param: key
    char query[64] = {0};
    char key_param[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "key", key_param, sizeof(key_param));
    }

    if (strlen(key_param) == 0 || strcmp(key_param, stored_key) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Gecersiz API anahtari\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Timer'i sifirla (restart butonu simule)
    esp_err_t err = timer_manager_restart();
    if (err == ESP_OK) {
        web_server_send_timer_event();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Timer sifirlandi\"}", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Timer sifirlanamadi\"}", HTTPD_RESP_USE_STRLEN);
}

// ============================================================
// Tetikleme Mail - Dosya Ek Yonetimi
// ============================================================

// POST /api/trigger-mail/upload - Dosya yukle (binary body, X-Group-Id + X-Filename header)
static esp_err_t api_trigger_mail_upload_handler(httpd_req_t *req)
{
    char group_str[8] = {0};
    char filename[64] = {0};

    if (httpd_req_get_hdr_value_str(req, "X-Group-Id", group_str, sizeof(group_str)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-Filename", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Group-Id ve X-Filename header gerekli");
        return ESP_FAIL;
    }

    int group_id = atoi(group_str);
    if (group_id < 0 || group_id >= 10 || strlen(filename) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Gecersiz grup veya dosya adi");
        return ESP_FAIL;
    }

    // Dosya adinda path traversal engelle
    if (strchr(filename, '/') || strchr(filename, '\\') || strstr(filename, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Gecersiz dosya adi");
        return ESP_FAIL;
    }

    // Max 2 MB
    if (req->content_len > 2 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Dosya cok buyuk (max 2MB)");
        return ESP_FAIL;
    }

    if (!ext_flash_is_user_data_mounted()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Harici flash kullanilamiyor\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Dizin olustur
    mkdir("/ext/mail", 0775);
    char dir_path[40];
    snprintf(dir_path, sizeof(dir_path), "/ext/mail/g%d", group_id);
    mkdir(dir_path, 0775);

    // Dosya yolu
    char file_path[128];
    snprintf(file_path, sizeof(file_path), "/ext/mail/g%d/%s", group_id, filename);

    FILE *f = fopen(file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Dosya acilamadi: %s", file_path);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Dosya olusturulamadi\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Chunk'lar halinde al ve yaz
    char *chunk = malloc(1024);
    if (!chunk) {
        fclose(f);
        remove(file_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bellek yetersiz");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    bool write_ok = true;
    while (remaining > 0) {
        int to_recv = (remaining > 1024) ? 1024 : remaining;
        int received = httpd_req_recv(req, chunk, to_recv);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            write_ok = false;
            break;
        }
        if (fwrite(chunk, 1, received, f) != (size_t)received) {
            write_ok = false;
            break;
        }
        remaining -= received;
    }

    fclose(f);
    free(chunk);

    if (!write_ok) {
        remove(file_path);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Dosya yazma hatasi\"}", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "Dosya yuklendi: %s (%d byte)", file_path, req->content_len);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/trigger-mail/files?group=X - Grup dosyalarini listele
static esp_err_t api_trigger_mail_files_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char group_str[8] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "group", group_str, sizeof(group_str));
    }
    int group_id = atoi(group_str);

    char dir_path[40];
    snprintf(dir_path, sizeof(dir_path), "/ext/mail/g%d", group_id);

    char *resp = malloc(2048);
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bellek yetersiz");
        return ESP_FAIL;
    }

    int pos = 0;
    pos += snprintf(resp + pos, 2048 - pos, "{\"files\":[");

    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        int first = 1;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type != DT_REG) continue;
            char fpath[300];
            snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, entry->d_name);
            struct stat st;
            long fsize = 0;
            if (stat(fpath, &st) == 0) fsize = (long)st.st_size;

            if (!first) pos += snprintf(resp + pos, 2048 - pos, ",");
            pos += snprintf(resp + pos, 2048 - pos,
                "{\"name\":\"%s\",\"size\":%ld}", entry->d_name, fsize);
            first = 0;
        }
        closedir(dir);
    }

    pos += snprintf(resp + pos, 2048 - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, resp, strlen(resp));
    free(resp);
    return r;
}

// POST /api/trigger-mail/file-delete - Dosya sil
static esp_err_t api_trigger_mail_file_delete_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Veri alinamadi");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char group_str[8] = {0};
    char filename[64] = {0};
    parse_json_string(buf, "group", group_str, sizeof(group_str));
    parse_json_string(buf, "name", filename, sizeof(filename));

    int group_id = atoi(group_str);

    // Path traversal engelle
    if (strchr(filename, '/') || strchr(filename, '\\') || strstr(filename, "..")) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Gecersiz dosya adi\"}", HTTPD_RESP_USE_STRLEN);
    }

    char file_path[128];
    snprintf(file_path, sizeof(file_path), "/ext/mail/g%d/%s", group_id, filename);

    if (remove(file_path) == 0) {
        ESP_LOGI(TAG, "Dosya silindi: %s", file_path);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Dosya silinemedi\"}", HTTPD_RESP_USE_STRLEN);
}

// ============================================================
// GUI OTA API
// ============================================================

// GET /api/gui-ota/info - Aktif slot, versiyon bilgileri
static esp_err_t api_gui_ota_info_handler(httpd_req_t *req)
{
    char active_ver[16] = {0};
    char inactive_ver[16] = {0};

    // Aktif slot versiyonu
    const char *active_mount = ext_flash_get_active_mount();
    {
        char path[64];
        snprintf(path, sizeof(path), "%s/config.json", active_mount);
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[256];
            size_t len = fread(buf, 1, sizeof(buf) - 1, f);
            buf[len] = '\0';
            fclose(f);
            parse_json_string(buf, "version", active_ver, sizeof(active_ver));
        }
    }

    // Inaktif slot versiyonu
    const char *inactive_mount = ext_flash_get_inactive_mount();
    {
        char path[64];
        snprintf(path, sizeof(path), "%s/config.json", inactive_mount);
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[256];
            size_t len = fread(buf, 1, sizeof(buf) - 1, f);
            buf[len] = '\0';
            fclose(f);
            parse_json_string(buf, "version", inactive_ver, sizeof(inactive_ver));
        }
    }

    char slot_char = ext_flash_get_active_slot();
    bool slot_b_has_data = (strlen(inactive_ver) > 0);

    char resp[384];
    snprintf(resp, sizeof(resp),
             "{\"active_slot\":\"%c\","
             "\"active_version\":\"%s\","
             "\"inactive_version\":\"%s\","
             "\"inactive_has_data\":%s,"
             "\"slot_b_mounted\":%s,"
             "\"busy\":%s}",
             slot_char,
             active_ver,
             inactive_ver,
             slot_b_has_data ? "true" : "false",
             ext_flash_is_slot_b_mounted() ? "true" : "false",
             gui_ota_is_busy() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/gui-ota/start - OTA baslatir
static esp_err_t api_gui_ota_start_handler(httpd_req_t *req)
{
    if (gui_ota_is_busy()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"OTA zaten calisiyor\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Body'den parametreleri oku (opsiyonel)
    gui_ota_params_t params;
    memset(&params, 0, sizeof(params));

    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        char source[16] = {0};
        parse_json_string(buf, "source", source, sizeof(source));
        if (strcmp(source, "custom") == 0) {
            params.use_custom_source = true;
            parse_json_string(buf, "repo_url", params.repo_url, sizeof(params.repo_url));
            parse_json_string(buf, "branch", params.branch, sizeof(params.branch));
        }
    }

    esp_err_t err = gui_ota_start(&params);
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"message\":\"OTA baslatildi\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"OTA baslatilamadi\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/gui-ota/status - Polling: durum, ilerleme, mesaj
static esp_err_t api_gui_ota_status_handler(httpd_req_t *req)
{
    gui_ota_status_t st;
    gui_ota_get_status(&st);

    // JSON escape message
    char esc_msg[256];
    json_escape(st.message, esc_msg, sizeof(esc_msg));

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"state\":%d,"
             "\"file_index\":%d,"
             "\"file_count\":%d,"
             "\"progress_pct\":%d,"
             "\"message\":\"%s\","
             "\"remote_version\":\"%s\","
             "\"current_version\":\"%s\"}",
             (int)st.state,
             st.file_index,
             st.file_count,
             st.progress_pct,
             esc_msg,
             st.remote_version,
             st.current_version);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/gui-ota/rollback - Onceki slot'a geri don
static esp_err_t api_gui_ota_rollback_handler(httpd_req_t *req)
{
    // Body'yi oku (bos olabilir)
    char buf[32];
    httpd_req_recv(req, buf, sizeof(buf) - 1);

    esp_err_t err = gui_ota_rollback();
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"active_slot\":\"%c\"}",
                 ext_flash_get_active_slot());
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"Diger slot'ta veri yok\"}");
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"Rollback basarisiz\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// ============================================================
// Firmware OTA API
// ============================================================

// GET /api/fw-ota/info
static esp_err_t api_fw_ota_info_handler(httpd_req_t *req)
{
    const char *running = fw_ota_get_running_label();
    const char *rollback_ver = fw_ota_get_rollback_version();
    bool can_rollback = fw_ota_can_rollback();
    bool busy = fw_ota_is_busy();

    fw_ota_status_t st;
    fw_ota_get_status(&st);

    char resp[384];
    snprintf(resp, sizeof(resp),
             "{\"running_partition\":\"%s\","
             "\"current_version\":\"%s\","
             "\"can_rollback\":%s,"
             "\"rollback_version\":\"%s\","
             "\"busy\":%s}",
             running,
             st.current_version,
             can_rollback ? "true" : "false",
             rollback_ver ? rollback_ver : "",
             busy ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/fw-ota/check
static esp_err_t api_fw_ota_check_handler(httpd_req_t *req)
{
    if (fw_ota_is_busy()) {
        const char *resp = "{\"status\":\"error\",\"message\":\"OTA zaten calisiyor\"}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, strlen(resp));
    }

    // Body oku (opsiyonel custom URL)
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len > 0) body[len] = '\0';

    // Custom URL parse et
    const char *custom_url = NULL;
    char url_buf[300] = {0};
    if (len > 0) {
        // source kontrolu
        char source[16] = {0};
        parse_json_string(body, "source", source, sizeof(source));
        if (strcmp(source, "custom") == 0) {
            parse_json_string(body, "url", url_buf, sizeof(url_buf));
            if (strlen(url_buf) > 0) {
                custom_url = url_buf;
            }
        }
    }

    esp_err_t err = fw_ota_check(custom_url);
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"message\":\"Kontrol basladi\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Baslatilamadi\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/fw-ota/start
static esp_err_t api_fw_ota_start_handler(httpd_req_t *req)
{
    esp_err_t err = fw_ota_start();
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"message\":\"Indirme basladi\"}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"Once versiyon kontrolu yapiniz\"}");
    } else {
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"Baslatilamadi\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// GET /api/fw-ota/status
static esp_err_t api_fw_ota_status_handler(httpd_req_t *req)
{
    fw_ota_status_t st;
    fw_ota_get_status(&st);

    // Mesaji JSON-safe yap
    char safe_msg[256];
    json_escape(st.message, safe_msg, sizeof(safe_msg));

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"state\":%d,"
             "\"progress_pct\":%d,"
             "\"message\":\"%s\","
             "\"remote_version\":\"%s\","
             "\"current_version\":\"%s\"}",
             (int)st.state,
             st.progress_pct,
             safe_msg,
             st.remote_version,
             st.current_version);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/fw-ota/rollback
static esp_err_t api_fw_ota_rollback_handler(httpd_req_t *req)
{
    esp_err_t err = fw_ota_rollback();
    char resp[128];
    if (err == ESP_OK) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"ok\",\"message\":\"Rollback ayarlandi, restart gerekli\"}");
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"Rollback partition bulunamadi\"}");
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"error\",\"message\":\"Rollback hatasi\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, strlen(resp));
}

// ============================================================
// Server Yonetim
// ============================================================

esp_err_t web_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server zaten calisiyor");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 50;
    config.stack_size = 8192;
    config.max_open_sockets = 13;   // varsayilan 7, SSE + dosya servisi icin arttirildi
    config.lru_purge_enable = true;  // soket dolunca en eski idle baglantiyi kapat

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Server baslatilamadi: %s", esp_err_to_name(ret));
        return ret;
    }

    // --- Ana sayfa ---
    const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler
    };
    httpd_register_uri_handler(s_server, &root);

    // --- API endpointleri ---
    const httpd_uri_t api_info = {
        .uri = "/api/system/info", .method = HTTP_GET, .handler = api_system_info_handler
    };
    httpd_register_uri_handler(s_server, &api_info);

    const httpd_uri_t api_wifi = {
        .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_handler
    };
    httpd_register_uri_handler(s_server, &api_wifi);

    const httpd_uri_t api_wifi_backup = {
        .uri = "/api/wifi/backup/save", .method = HTTP_POST, .handler = api_wifi_backup_save_handler
    };
    httpd_register_uri_handler(s_server, &api_wifi_backup);

    const httpd_uri_t api_wifi_cfg = {
        .uri = "/api/wifi/config", .method = HTTP_GET, .handler = api_wifi_config_handler
    };
    httpd_register_uri_handler(s_server, &api_wifi_cfg);

    const httpd_uri_t api_wifi_ap = {
        .uri = "/api/wifi/ap/set", .method = HTTP_POST, .handler = api_wifi_ap_set_handler
    };
    httpd_register_uri_handler(s_server, &api_wifi_ap);

    // --- Timer API ---
    const httpd_uri_t api_timer_get = {
        .uri = "/api/timer/get", .method = HTTP_GET, .handler = api_timer_get_handler
    };
    httpd_register_uri_handler(s_server, &api_timer_get);

    const httpd_uri_t api_timer_set = {
        .uri = "/api/timer/set", .method = HTTP_POST, .handler = api_timer_set_handler
    };
    httpd_register_uri_handler(s_server, &api_timer_set);

    // --- SMTP API ---
    const httpd_uri_t api_smtp_get = {
        .uri = "/api/smtp/get", .method = HTTP_GET, .handler = api_smtp_get_handler
    };
    httpd_register_uri_handler(s_server, &api_smtp_get);

    const httpd_uri_t api_smtp_save = {
        .uri = "/api/smtp/save", .method = HTTP_POST, .handler = api_smtp_save_handler
    };
    httpd_register_uri_handler(s_server, &api_smtp_save);

    const httpd_uri_t api_smtp_test = {
        .uri = "/api/smtp/test", .method = HTTP_POST, .handler = api_smtp_test_handler
    };
    httpd_register_uri_handler(s_server, &api_smtp_test);

    // --- System Detail + Logs API ---
    const httpd_uri_t api_sys_detail = {
        .uri = "/api/system/detail", .method = HTTP_GET, .handler = api_system_detail_handler
    };
    httpd_register_uri_handler(s_server, &api_sys_detail);

    const httpd_uri_t api_logs = {
        .uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_handler
    };
    httpd_register_uri_handler(s_server, &api_logs);

    // --- Relay API ---
    const httpd_uri_t api_relay_get = {
        .uri = "/api/relay/get", .method = HTTP_GET, .handler = api_relay_get_handler
    };
    httpd_register_uri_handler(s_server, &api_relay_get);

    const httpd_uri_t api_relay_set = {
        .uri = "/api/relay/set", .method = HTTP_POST, .handler = api_relay_set_handler
    };
    httpd_register_uri_handler(s_server, &api_relay_set);

    const httpd_uri_t api_relay_test = {
        .uri = "/api/relay/test", .method = HTTP_POST, .handler = api_relay_test_handler
    };
    httpd_register_uri_handler(s_server, &api_relay_test);

    // --- Device Control API ---
    const httpd_uri_t api_restart = {
        .uri = "/api/device/restart", .method = HTTP_POST, .handler = api_device_restart_handler
    };
    httpd_register_uri_handler(s_server, &api_restart);

    const httpd_uri_t api_factory_reset = {
        .uri = "/api/device/factory-reset", .method = HTTP_POST, .handler = api_device_factory_reset_handler
    };
    httpd_register_uri_handler(s_server, &api_factory_reset);

    // --- Early Mail API ---
    const httpd_uri_t api_early_mail_get = {
        .uri = "/api/early-mail/get", .method = HTTP_GET, .handler = api_early_mail_get_handler
    };
    httpd_register_uri_handler(s_server, &api_early_mail_get);

    const httpd_uri_t api_early_mail_save = {
        .uri = "/api/early-mail/save", .method = HTTP_POST, .handler = api_early_mail_save_handler
    };
    httpd_register_uri_handler(s_server, &api_early_mail_save);

    // --- Trigger Mail API ---
    const httpd_uri_t api_trig_mail_get = {
        .uri = "/api/trigger-mail/get", .method = HTTP_GET, .handler = api_trigger_mail_get_handler
    };
    httpd_register_uri_handler(s_server, &api_trig_mail_get);

    const httpd_uri_t api_trig_mail_save = {
        .uri = "/api/trigger-mail/save", .method = HTTP_POST, .handler = api_trigger_mail_save_handler
    };
    httpd_register_uri_handler(s_server, &api_trig_mail_save);

    const httpd_uri_t api_trig_mail_del = {
        .uri = "/api/trigger-mail/delete", .method = HTTP_POST, .handler = api_trigger_mail_delete_handler
    };
    httpd_register_uri_handler(s_server, &api_trig_mail_del);

    // --- Trigger Mail File API ---
    const httpd_uri_t api_trig_mail_upload = {
        .uri = "/api/trigger-mail/upload", .method = HTTP_POST, .handler = api_trigger_mail_upload_handler
    };
    httpd_register_uri_handler(s_server, &api_trig_mail_upload);

    const httpd_uri_t api_trig_mail_files = {
        .uri = "/api/trigger-mail/files", .method = HTTP_GET, .handler = api_trigger_mail_files_handler
    };
    httpd_register_uri_handler(s_server, &api_trig_mail_files);

    const httpd_uri_t api_trig_mail_file_del = {
        .uri = "/api/trigger-mail/file-delete", .method = HTTP_POST, .handler = api_trigger_mail_file_delete_handler
    };
    httpd_register_uri_handler(s_server, &api_trig_mail_file_del);

    // --- Actions API ---
    const httpd_uri_t api_actions_get = {
        .uri = "/api/actions/get", .method = HTTP_GET, .handler = api_actions_get_handler
    };
    httpd_register_uri_handler(s_server, &api_actions_get);

    const httpd_uri_t api_actions_save = {
        .uri = "/api/actions/save", .method = HTTP_POST, .handler = api_actions_save_handler
    };
    httpd_register_uri_handler(s_server, &api_actions_save);

    // --- Telegram API ---
    const httpd_uri_t api_telegram_get = {
        .uri = "/api/telegram/get", .method = HTTP_GET, .handler = api_telegram_get_handler
    };
    httpd_register_uri_handler(s_server, &api_telegram_get);

    const httpd_uri_t api_telegram_save = {
        .uri = "/api/telegram/save", .method = HTTP_POST, .handler = api_telegram_save_handler
    };
    httpd_register_uri_handler(s_server, &api_telegram_save);

    const httpd_uri_t api_telegram_test = {
        .uri = "/api/telegram/test", .method = HTTP_POST, .handler = api_telegram_test_handler
    };
    httpd_register_uri_handler(s_server, &api_telegram_test);

    // --- Reset API ---
    const httpd_uri_t api_reset_cfg_get = {
        .uri = "/api/reset/config", .method = HTTP_GET, .handler = api_reset_config_get_handler
    };
    httpd_register_uri_handler(s_server, &api_reset_cfg_get);

    const httpd_uri_t api_reset_cfg_save = {
        .uri = "/api/reset/config", .method = HTTP_POST, .handler = api_reset_config_save_handler
    };
    httpd_register_uri_handler(s_server, &api_reset_cfg_save);

    const httpd_uri_t api_reset_generate = {
        .uri = "/api/reset/generate", .method = HTTP_POST, .handler = api_reset_generate_handler
    };
    httpd_register_uri_handler(s_server, &api_reset_generate);

    const httpd_uri_t api_reset = {
        .uri = "/api/reset", .method = HTTP_GET, .handler = api_reset_handler
    };
    httpd_register_uri_handler(s_server, &api_reset);

    // --- GUI OTA API ---
    const httpd_uri_t api_gui_ota_info = {
        .uri = "/api/gui-ota/info", .method = HTTP_GET, .handler = api_gui_ota_info_handler
    };
    httpd_register_uri_handler(s_server, &api_gui_ota_info);

    const httpd_uri_t api_gui_ota_start = {
        .uri = "/api/gui-ota/start", .method = HTTP_POST, .handler = api_gui_ota_start_handler
    };
    httpd_register_uri_handler(s_server, &api_gui_ota_start);

    const httpd_uri_t api_gui_ota_status = {
        .uri = "/api/gui-ota/status", .method = HTTP_GET, .handler = api_gui_ota_status_handler
    };
    httpd_register_uri_handler(s_server, &api_gui_ota_status);

    const httpd_uri_t api_gui_ota_rollback = {
        .uri = "/api/gui-ota/rollback", .method = HTTP_POST, .handler = api_gui_ota_rollback_handler
    };
    httpd_register_uri_handler(s_server, &api_gui_ota_rollback);

    // --- Firmware OTA API ---
    const httpd_uri_t api_fw_ota_info = {
        .uri = "/api/fw-ota/info", .method = HTTP_GET, .handler = api_fw_ota_info_handler
    };
    httpd_register_uri_handler(s_server, &api_fw_ota_info);

    const httpd_uri_t api_fw_ota_check = {
        .uri = "/api/fw-ota/check", .method = HTTP_POST, .handler = api_fw_ota_check_handler
    };
    httpd_register_uri_handler(s_server, &api_fw_ota_check);

    const httpd_uri_t api_fw_ota_start = {
        .uri = "/api/fw-ota/start", .method = HTTP_POST, .handler = api_fw_ota_start_handler
    };
    httpd_register_uri_handler(s_server, &api_fw_ota_start);

    const httpd_uri_t api_fw_ota_status = {
        .uri = "/api/fw-ota/status", .method = HTTP_GET, .handler = api_fw_ota_status_handler
    };
    httpd_register_uri_handler(s_server, &api_fw_ota_status);

    const httpd_uri_t api_fw_ota_rollback = {
        .uri = "/api/fw-ota/rollback", .method = HTTP_POST, .handler = api_fw_ota_rollback_handler
    };
    httpd_register_uri_handler(s_server, &api_fw_ota_rollback);

    // --- SSE Events ---
    const httpd_uri_t api_events = {
        .uri = "/api/events", .method = HTTP_GET, .handler = api_events_handler
    };
    httpd_register_uri_handler(s_server, &api_events);

    // --- Statik dosyalar (harici flash + gomulu fallback) ---
    const httpd_uri_t ext_files = {
        .uri = "/ext/*", .method = HTTP_GET, .handler = ext_file_handler
    };
    httpd_register_uri_handler(s_server, &ext_files);

    // SSE gonderim task'i olustur (thread-safe SSE icin)
    if (!s_sse_task_handle) {
        xTaskCreate(sse_send_task, "sse_send", 3072, NULL, 5, &s_sse_task_handle);
    }

    ESP_LOGI(TAG, "Web server baslatildi (port: %d)", config.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Web server durduruldu");
    return ret;
}
