/**
 * @file wifi_manager.c
 * @brief WiFi management - AP + STA dual mode for ESP32-C6
 */

#include "wifi_manager.h"
#include "device_id.h"
#include "config_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mdns.h"
#include "lwip/inet.h"
#include "lwip/dns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi";

/* Event bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_SCAN_DONE_BIT  BIT2

/* State */
static EventGroupHandle_t s_event_group = NULL;
static esp_netif_t *s_netif_ap  = NULL;
static esp_netif_t *s_netif_sta = NULL;
static wifi_state_t s_state     = WIFI_STATE_IDLE;
static int  s_retry             = 0;
static char s_sta_ip[16]        = "0.0.0.0";
static char s_ap_ip[16]         = "192.168.4.1";
static char s_hostname[32]      = {0};
static uint8_t s_ap_clients     = 0;
static bool s_inited            = false;

/* Pending async connect */
static char s_pend_ssid[33]     = {0};
static char s_pend_pass[65]     = {0};
static TaskHandle_t s_conn_task = NULL;

/* ── Event handler ────────────────────────────────────── */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) s_ap_clients--;
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_state = WIFI_STATE_CONNECTING;   /* waiting for IP */
            s_retry = 0;
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = data;
            ESP_LOGW(TAG, "STA disc reason:%d", ev->reason);
            strcpy(s_sta_ip, "0.0.0.0");

            if (s_state == WIFI_STATE_CONNECTING || s_state == WIFI_STATE_CONNECTED) {
                if (s_retry < WIFI_MAX_RETRY) {
                    s_retry++;
                    ESP_LOGI(TAG, "Retry %d/%d", s_retry, WIFI_MAX_RETRY);
                    esp_wifi_connect();
                } else {
                    s_state = WIFI_STATE_FAILED;
                    xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
                }
            } else {
                s_state = WIFI_STATE_DISCONNECTED;
            }
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            xEventGroupSetBits(s_event_group, WIFI_SCAN_DONE_BIT);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_sta_ip);
        ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ev->ip_info.gw));

        /* Log current DNS servers from DHCP */
        const ip_addr_t *d0 = dns_getserver(0);
        const ip_addr_t *d1 = dns_getserver(1);
        ESP_LOGI(TAG, "DHCP DNS0: %s", ipaddr_ntoa(d0));
        ESP_LOGI(TAG, "DHCP DNS1: %s", ipaddr_ntoa(d1));

        /* If DHCP didn't provide DNS, use gateway as DNS relay */
        if (ip_addr_isany(d0)) {
            ip_addr_t gw_addr;
            IP_ADDR4(&gw_addr,
                      ip4_addr1_16(&ev->ip_info.gw),
                      ip4_addr2_16(&ev->ip_info.gw),
                      ip4_addr3_16(&ev->ip_info.gw),
                      ip4_addr4_16(&ev->ip_info.gw));
            dns_setserver(0, &gw_addr);
            ESP_LOGI(TAG, "No DHCP DNS, using gateway as DNS");
        }

        /* Set Google DNS as secondary/fallback */
        ip_addr_t gdns;
        IP_ADDR4(&gdns, 8, 8, 8, 8);
        dns_setserver(1, &gdns);
        ESP_LOGI(TAG, "DNS configured");

        s_state = WIFI_STATE_CONNECTED;
        s_retry = 0;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        strcpy(s_sta_ip, "0.0.0.0");
    }
}

/* ── mDNS ─────────────────────────────────────────────── */

static void setup_mdns(void)
{
    // Check for custom hostname from config
    app_wifi_config_t wifi_cfg;
    if (config_load_wifi(&wifi_cfg) == ESP_OK && strlen(wifi_cfg.mdns_hostname) > 0) {
        strncpy(s_hostname, wifi_cfg.mdns_hostname, sizeof(s_hostname) - 1);
    } else {
        strncpy(s_hostname, device_id_get(), sizeof(s_hostname) - 1);
    }
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set("LebensSpur");
    mdns_service_add("LebensSpur", "_http", "_tcp", 80, NULL, 0);
}

int wifi_manager_set_hostname(const char *hostname)
{
    if (!hostname) return ESP_ERR_INVALID_ARG;
    strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
    s_hostname[sizeof(s_hostname) - 1] = '\0';
    return mdns_hostname_set(s_hostname);
}

/* ── Init ─────────────────────────────────────────────── */

int wifi_manager_init(void)
{
    if (s_inited) return ESP_OK;
    if (!device_id_is_valid()) return ESP_ERR_INVALID_STATE;

    s_event_group = xEventGroupCreate();
    if (!s_event_group) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());

    s_netif_ap  = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    setup_mdns();

    s_inited = true;
    return ESP_OK;
}

/* ── AP ───────────────────────────────────────────────── */

int wifi_manager_start_ap(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    const char *id = device_id_get();

    wifi_config_t ap = {
        .ap = {
            .channel        = WIFI_AP_CHANNEL,
            .max_connection  = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg.required = false,
        },
    };
    strncpy((char *)ap.ap.ssid, id, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(id);
    strncpy((char *)ap.ap.password, WIFI_AP_PASSWORD, sizeof(ap.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

int wifi_manager_stop_ap(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    wifi_mode_t m;
    esp_wifi_get_mode(&m);
    if (m == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    } else if (m == WIFI_MODE_AP) {
        esp_wifi_stop();
    }
    s_ap_clients = 0;
    return ESP_OK;
}

/* ── STA connect (blocking) ───────────────────────────── */

static void sta_configure(const char *ssid, const char *password)
{
    wifi_config_t c = {0};
    strncpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid) - 1);
    if (password && *password)
        strncpy((char *)c.sta.password, password, sizeof(c.sta.password) - 1);
    c.sta.pmf_cfg.capable  = true;
    c.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &c);
}

int wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;

    /* Disconnect cleanly first */
    s_state = WIFI_STATE_IDLE;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));

    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_state = WIFI_STATE_CONNECTING;
    s_retry = 0;

    sta_configure(ssid, password);

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        s_state = WIFI_STATE_FAILED;
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_SEC * 1000));

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    if (bits & WIFI_FAIL_BIT)      return ESP_FAIL;

    s_state = WIFI_STATE_FAILED;
    return ESP_ERR_TIMEOUT;
}

/* ── STA connect (async, for setup page) ──────────────── */

static void async_connect_task(void *arg)
{
    /*  Wait so that the HTTP response reaches the client
        BEFORE we potentially change WiFi channel */
    vTaskDelay(pdMS_TO_TICKS(800));

    /* 1. Put STA in idle so disconnect-event won't auto-retry old creds */
    s_state = WIFI_STATE_IDLE;
    s_retry = 0;

    /* 2. Disconnect whatever was connected */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 3. Configure new credentials */
    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    sta_configure(s_pend_ssid, s_pend_pass);

    /* 4. NOW go to CONNECTING so event handler will do retries */
    s_state = WIFI_STATE_CONNECTING;
    s_retry = 0;

    /* 5. Connect */
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "connect err: %s", esp_err_to_name(ret));
        s_state = WIFI_STATE_FAILED;
    }

    memset(s_pend_pass, 0, sizeof(s_pend_pass));
    s_conn_task = NULL;
    vTaskDelete(NULL);
}

int wifi_manager_connect_async(const char *ssid, const char *password)
{
    if (!s_inited)   return ESP_ERR_INVALID_STATE;
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;

    /* Kill any pending task */
    if (s_conn_task) {
        vTaskDelete(s_conn_task);
        s_conn_task = NULL;
    }

    /* Store credentials */
    memset(s_pend_ssid, 0, sizeof(s_pend_ssid));
    memset(s_pend_pass, 0, sizeof(s_pend_pass));
    strncpy(s_pend_ssid, ssid, sizeof(s_pend_ssid) - 1);
    if (password && *password)
        strncpy(s_pend_pass, password, sizeof(s_pend_pass) - 1);

    if (xTaskCreate(async_connect_task, "wcon", 4096, NULL, 5, &s_conn_task) != pdPASS) {
        s_state = WIFI_STATE_FAILED;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ── Disconnect ───────────────────────────────────────── */

int wifi_manager_disconnect(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    s_state = WIFI_STATE_IDLE;
    strcpy(s_sta_ip, "0.0.0.0");
    esp_wifi_disconnect();
    return ESP_OK;
}

/* ── Scan ─────────────────────────────────────────────── */

int wifi_manager_scan(wifi_scan_result_t *out, uint16_t max)
{
    if (!s_inited || !out) return -1;

    xEventGroupClearBits(s_event_group, WIFI_SCAN_DONE_BIT);

    wifi_scan_config_t sc = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    if (esp_wifi_scan_start(&sc, false) != ESP_OK) return -1;

    EventBits_t bits = xEventGroupWaitBits(
        s_event_group, WIFI_SCAN_DONE_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WIFI_SCAN_DONE_BIT)) {
        esp_wifi_scan_stop();
        return -1;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;

    uint16_t cnt = (n < max) ? n : max;
    wifi_ap_record_t *recs = malloc(cnt * sizeof(wifi_ap_record_t));
    if (!recs) return -1;

    esp_wifi_scan_get_ap_records(&cnt, recs);
    for (int i = 0; i < cnt; i++) {
        strncpy(out[i].ssid, (char *)recs[i].ssid, sizeof(out[i].ssid) - 1);
        out[i].ssid[sizeof(out[i].ssid) - 1] = '\0';
        out[i].rssi     = recs[i].rssi;
        out[i].authmode = recs[i].authmode;
    }
    free(recs);
    return cnt;
}

/* ── Status ───────────────────────────────────────────── */

int wifi_manager_get_status(wifi_status_t *st)
{
    if (!s_inited || !st) return ESP_ERR_INVALID_STATE;
    memset(st, 0, sizeof(*st));

    wifi_mode_t m;
    esp_wifi_get_mode(&m);

    st->ap_active     = (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA);
    st->sta_connected = (s_state == WIFI_STATE_CONNECTED);
    st->sta_state     = s_state;
    st->ap_clients    = s_ap_clients;

    if (st->ap_active) {
        strncpy(st->ap_ssid, device_id_get(), sizeof(st->ap_ssid) - 1);
        strncpy(st->ap_ip,   s_ap_ip,         sizeof(st->ap_ip)   - 1);
    }
    if (st->sta_connected) {
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            strncpy(st->sta_ssid, (char *)info.ssid, sizeof(st->sta_ssid) - 1);
            st->sta_rssi = info.rssi;
        }
        strncpy(st->sta_ip, s_sta_ip, sizeof(st->sta_ip) - 1);
    }
    return ESP_OK;
}

bool wifi_manager_is_connected(void)           { return s_state == WIFI_STATE_CONNECTED; }
const char* wifi_manager_get_sta_ip(void)      { return s_sta_ip; }
const char* wifi_manager_get_ap_ip(void)       { return s_ap_ip; }
const char* wifi_manager_get_hostname(void)    { return s_hostname; }
