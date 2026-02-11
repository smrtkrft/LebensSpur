/**
 * @file wifi_manager.c
 * @brief WiFi management implementation for LebensSpur ESP32-C6
 * 
 * Features:
 * - AP mode: SSID = Device ID, Password = smartkraft
 * - STA mode: Connect to external networks
 * - mDNS: {device_id}.local
 * - Concurrent AP+STA operation
 */

#include "wifi_manager.h"
#include "device_id.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mdns.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

/** Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_SCAN_DONE_BIT  BIT2

/** WiFi event group */
static EventGroupHandle_t s_wifi_event_group = NULL;

/** Network interfaces */
static esp_netif_t *s_netif_ap = NULL;
static esp_netif_t *s_netif_sta = NULL;

/** Connection state */
static wifi_state_t s_sta_state = WIFI_STATE_IDLE;
static int s_retry_count = 0;
static const int s_max_retry = 3;

/** IP addresses */
static char s_sta_ip[16] = "0.0.0.0";
static char s_ap_ip[16] = "192.168.4.1";

/** Hostname (device ID) */
static char s_hostname[32] = {0};

/** AP client count */
static uint8_t s_ap_clients = 0;

/** Initialization flag */
static bool s_initialized = false;

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started - SSID: %s, Password: %s", 
                         device_id_get(), WIFI_AP_PASSWORD);
                break;
                
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                break;
                
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                s_ap_clients++;
                ESP_LOGI(TAG, "Client connected to AP - MAC: " MACSTR ", AID: %d, Total: %d",
                         MAC2STR(event->mac), event->aid, s_ap_clients);
                break;
            }
            
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                if (s_ap_clients > 0) s_ap_clients--;
                ESP_LOGI(TAG, "Client disconnected from AP - MAC: " MACSTR ", Total: %d",
                         MAC2STR(event->mac), s_ap_clients);
                break;
            }
            
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started");
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA connected to AP");
                s_sta_state = WIFI_STATE_CONNECTING; // Wait for IP
                s_retry_count = 0;
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "STA disconnected, reason: %d", event->reason);
                
                strcpy(s_sta_ip, "0.0.0.0");
                
                if (s_sta_state == WIFI_STATE_CONNECTING || s_sta_state == WIFI_STATE_CONNECTED) {
                    if (s_retry_count < s_max_retry) {
                        s_retry_count++;
                        ESP_LOGI(TAG, "Retrying connection (%d/%d)...", s_retry_count, s_max_retry);
                        esp_wifi_connect();
                    } else {
                        ESP_LOGE(TAG, "Connection failed after %d retries", s_max_retry);
                        s_sta_state = WIFI_STATE_FAILED;
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                } else {
                    s_sta_state = WIFI_STATE_DISCONNECTED;
                }
                break;
            }
            
            case WIFI_EVENT_SCAN_DONE:
                ESP_LOGI(TAG, "WiFi scan completed");
                xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
                break;
                
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
                ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip);
                s_sta_state = WIFI_STATE_CONNECTED;
                s_retry_count = 0;
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                break;
            }
            
            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "STA lost IP");
                strcpy(s_sta_ip, "0.0.0.0");
                break;
                
            default:
                break;
        }
    }
}

/**
 * @brief Initialize mDNS with device ID as hostname
 */
static esp_err_t init_mdns(void)
{
    // Hostname = device ID (without modification)
    strncpy(s_hostname, device_id_get(), sizeof(s_hostname) - 1);
    
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set hostname (will be accessible as {hostname}.local)
    ret = mdns_hostname_set(s_hostname);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set instance name
    ret = mdns_instance_name_set("LebensSpur Device");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name set failed");
    }
    
    // Add HTTP service
    ret = mdns_service_add("LebensSpur Web", "_http", "_tcp", 80, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service add failed");
    }
    
    ESP_LOGI(TAG, "mDNS initialized - hostname: %s.local", s_hostname);
    
    return ESP_OK;
}

int wifi_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    // Check device ID is ready
    if (!device_id_is_valid()) {
        ESP_LOGE(TAG, "Device ID not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Initializing WiFi manager...");
    
    // Create event group
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create default network interfaces
    s_netif_ap = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();
    
    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    
    // Set WiFi mode to AP+STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    // Initialize mDNS
    init_mdns();
    
    s_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized");
    
    return ESP_OK;
}

int wifi_manager_start_ap(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    const char *device_id = device_id_get();
    
    // Configure AP
    wifi_config_t ap_config = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    
    // Set SSID = Device ID
    strncpy((char *)ap_config.ap.ssid, device_id, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(device_id);
    
    // Set password = smartkraft
    strncpy((char *)ap_config.ap.password, WIFI_AP_PASSWORD, sizeof(ap_config.ap.password) - 1);
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "AP started:");
    ESP_LOGI(TAG, "  SSID: %s", device_id);
    ESP_LOGI(TAG, "  Password: %s", WIFI_AP_PASSWORD);
    ESP_LOGI(TAG, "  IP: %s", s_ap_ip);
    ESP_LOGI(TAG, "  mDNS: %s.local", s_hostname);
    
    return ESP_OK;
}

int wifi_manager_stop_ap(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Note: This stops WiFi entirely in AP+STA mode
    // For partial stop, would need to change mode to STA only
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    
    if (mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    } else if (mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_stop());
    }
    
    s_ap_clients = 0;
    ESP_LOGI(TAG, "AP stopped");
    
    return ESP_OK;
}

int wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Connecting to: %s", ssid);
    
    // Clear previous state
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_sta_state = WIFI_STATE_CONNECTING;
    s_retry_count = 0;
    
    // Configure STA
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    
    if (password != NULL && strlen(password) > 0) {
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    }
    
    // Enable PMF if available
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    
    // Ensure WiFi is started
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    
    // Start connection
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed: %s", esp_err_to_name(ret));
        s_sta_state = WIFI_STATE_FAILED;
        return ret;
    }
    
    // Wait for connection result
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,  // Clear bits
        pdFALSE, // Wait for any bit
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_SEC * 1000)
    );
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s, IP: %s", ssid, s_sta_ip);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to %s", ssid);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Connection timeout");
        s_sta_state = WIFI_STATE_FAILED;
        return ESP_ERR_TIMEOUT;
    }
}

int wifi_manager_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_sta_state = WIFI_STATE_IDLE;
    strcpy(s_sta_ip, "0.0.0.0");
    
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Disconnect failed: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Disconnected from WiFi");
    
    return ESP_OK;
}

int wifi_manager_scan(wifi_scan_result_t *results, uint16_t max_results)
{
    if (!s_initialized || results == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting WiFi scan...");
    
    // Clear scan done bit
    xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    
    // Configure scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    
    // Start scan
    esp_err_t ret = esp_wifi_scan_start(&scan_config, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        return -1;
    }
    
    // Wait for scan completion
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_SCAN_DONE_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(10000)
    );
    
    if (!(bits & WIFI_SCAN_DONE_BIT)) {
        ESP_LOGW(TAG, "Scan timeout");
        esp_wifi_scan_stop();
        return -1;
    }
    
    // Get results
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    if (ap_count == 0) {
        ESP_LOGI(TAG, "No networks found");
        return 0;
    }
    
    uint16_t count = (ap_count < max_results) ? ap_count : max_results;
    wifi_ap_record_t *ap_records = malloc(count * sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed");
        return -1;
    }
    
    ret = esp_wifi_scan_get_ap_records(&count, ap_records);
    if (ret != ESP_OK) {
        free(ap_records);
        return -1;
    }
    
    // Copy results
    for (int i = 0; i < count; i++) {
        strncpy(results[i].ssid, (char *)ap_records[i].ssid, sizeof(results[i].ssid) - 1);
        results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
        results[i].rssi = ap_records[i].rssi;
        results[i].authmode = ap_records[i].authmode;
    }
    
    free(ap_records);
    
    ESP_LOGI(TAG, "Scan found %d networks", count);
    
    return count;
}

int wifi_manager_get_status(wifi_status_t *status)
{
    if (!s_initialized || status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    memset(status, 0, sizeof(wifi_status_t));
    
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    
    status->ap_active = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    status->sta_connected = (s_sta_state == WIFI_STATE_CONNECTED);
    status->sta_state = s_sta_state;
    status->ap_clients = s_ap_clients;
    
    // AP info
    if (status->ap_active) {
        strncpy(status->ap_ssid, device_id_get(), sizeof(status->ap_ssid) - 1);
        strncpy(status->ap_ip, s_ap_ip, sizeof(status->ap_ip) - 1);
    }
    
    // STA info
    if (status->sta_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            strncpy(status->sta_ssid, (char *)ap_info.ssid, sizeof(status->sta_ssid) - 1);
            status->sta_rssi = ap_info.rssi;
        }
        strncpy(status->sta_ip, s_sta_ip, sizeof(status->sta_ip) - 1);
    }
    
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_sta_state == WIFI_STATE_CONNECTED;
}

const char* wifi_manager_get_sta_ip(void)
{
    return s_sta_ip;
}

const char* wifi_manager_get_ap_ip(void)
{
    return s_ap_ip;
}

const char* wifi_manager_get_hostname(void)
{
    return s_hostname;
}
