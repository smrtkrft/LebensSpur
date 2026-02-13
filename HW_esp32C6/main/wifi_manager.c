/**
 * WiFi Manager - AP + STA Yönetimi
 *
 * AP SSID = device_id (LS-XXXXXXXXXX) benzersiz.
 * STA: primary + secondary WiFi, otomatik geçiş.
 *
 * Bağımlılık: config_manager, device_id
 * Katman: 3 (İletişim)
 */

#include "wifi_manager.h"
#include "config_manager.h"
#include "device_id.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"
#include <string.h>

static const char *TAG = "WIFI";

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_initialized = false;
static bool s_connected = false;
static char s_ip_addr[16] = "0.0.0.0";
static char s_ap_ssid[32] = {0};
static wifi_event_cb_t s_event_cb = NULL;

// Config'den yüklenen STA ayarları
static ls_wifi_config_t s_wifi_cfg;
static bool s_using_secondary = false;

// ============================================================================
// Event Handler
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA baslatildi");
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA baglandi");
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                ESP_LOGW(TAG, "STA baglanti kesildi");
                s_connected = false;
                strcpy(s_ip_addr, "0.0.0.0");
                if (s_event_cb) s_event_cb(false);

                // Otomatik yeniden bağlan
                esp_wifi_connect();
                break;
            }

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
                ESP_LOGI(TAG, "AP: cihaz baglandi %02X:%02X:%02X:%02X:%02X:%02X",
                         e->mac[0], e->mac[1], e->mac[2],
                         e->mac[3], e->mac[4], e->mac[5]);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
                ESP_LOGI(TAG, "AP: cihaz ayrildi %02X:%02X:%02X:%02X:%02X:%02X",
                         e->mac[0], e->mac[1], e->mac[2],
                         e->mac[3], e->mac[4], e->mac[5]);
                break;
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "IP alindi: %s", s_ip_addr);

        // DNS ayarla (APSTA modunda DHCP DNS sorunlu olabilir)
        ip_addr_t dns1, dns2;
        IP_ADDR4(&dns1, 8, 8, 8, 8);
        IP_ADDR4(&dns2, 8, 8, 4, 4);
        dns_setserver(0, &dns1);
        dns_setserver(1, &dns2);

        s_connected = true;
        if (s_event_cb) s_event_cb(true);
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    // Config yükle
    config_load_wifi(&s_wifi_cfg);

    // AP SSID = device_id (LS-XXXXXXXXXX)
    const char *dev_id = device_id_get();
    strncpy(s_ap_ssid, dev_id, sizeof(s_ap_ssid) - 1);

    // TCP/IP stack
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netif init hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    // AP ve STA arayüzleri
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // WiFi başlat
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi init hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Event handler
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    // AP yapılandırması - SSID = device_id
    wifi_config_t ap_cfg = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    strncpy((char *)ap_cfg.ap.password, WIFI_AP_PASS, sizeof(ap_cfg.ap.password));

    // Mod: config'e göre APSTA veya sadece STA
    wifi_mode_t mode = s_wifi_cfg.ap_mode_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_wifi_set_mode(mode);

    if (s_wifi_cfg.ap_mode_enabled) {
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }

    esp_wifi_start();

    s_initialized = true;
    ESP_LOGI(TAG, "OK - AP SSID: %s, mod: %s",
             s_ap_ssid,
             mode == WIFI_MODE_APSTA ? "APSTA" : "STA");

    // Config'de SSID varsa otomatik bağlan
    if (s_wifi_cfg.primary_ssid[0] != '\0') {
        wifi_manager_connect_from_config();
    }

    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!ssid) return ESP_ERR_INVALID_ARG;

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    }

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    ESP_LOGI(TAG, "Baglaniliyor: %s", ssid);

    return esp_wifi_connect();
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return esp_wifi_disconnect();
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip_addr;
}

const char *wifi_manager_get_ap_ip(void)
{
    return "192.168.4.1";
}

const char *wifi_manager_get_ap_ssid(void)
{
    return s_ap_ssid;
}

void wifi_manager_set_callback(wifi_event_cb_t callback)
{
    s_event_cb = callback;
}

esp_err_t wifi_manager_scan(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    return esp_wifi_scan_start(&scan, true);
}

esp_err_t wifi_manager_scan_get_results(wifi_ap_record_t *records, uint16_t *count)
{
    if (!records || !count) return ESP_ERR_INVALID_ARG;
    return esp_wifi_scan_get_ap_records(count, records);
}

uint16_t wifi_manager_scan_get_count(void)
{
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    return count;
}

esp_err_t wifi_manager_connect_from_config(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    // Önce primary dene
    if (s_wifi_cfg.primary_ssid[0] != '\0') {
        s_using_secondary = false;
        ESP_LOGI(TAG, "Primary WiFi: %s", s_wifi_cfg.primary_ssid);
        return wifi_manager_connect(s_wifi_cfg.primary_ssid,
                                     s_wifi_cfg.primary_password);
    }

    // Primary yoksa secondary dene
    if (s_wifi_cfg.secondary_ssid[0] != '\0') {
        s_using_secondary = true;
        ESP_LOGI(TAG, "Secondary WiFi: %s", s_wifi_cfg.secondary_ssid);
        return wifi_manager_connect(s_wifi_cfg.secondary_ssid,
                                     s_wifi_cfg.secondary_password);
    }

    ESP_LOGW(TAG, "Config'de WiFi SSID yok");
    return ESP_ERR_NOT_FOUND;
}

void wifi_manager_print_info(void)
{
    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ AP SSID:   %s", s_ap_ssid);
    ESP_LOGI(TAG, "│ AP IP:     192.168.4.1");
    ESP_LOGI(TAG, "│ STA:       %s", s_connected ? "BAGLI" : "BAGLI DEGIL");

    if (s_connected) {
        ESP_LOGI(TAG, "│ STA IP:    %s", s_ip_addr);
        ESP_LOGI(TAG, "│ Ag:        %s (%s)",
                 s_using_secondary ? s_wifi_cfg.secondary_ssid : s_wifi_cfg.primary_ssid,
                 s_using_secondary ? "secondary" : "primary");
    }

    ESP_LOGI(TAG, "│ Config:    P='%s' S='%s'",
             s_wifi_cfg.primary_ssid, s_wifi_cfg.secondary_ssid);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
