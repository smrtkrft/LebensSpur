/**
 * @file wifi_manager.h
 * @brief WiFi management for LebensSpur ESP32-C6
 * 
 * Manages WiFi AP and STA modes concurrently.
 * AP SSID = Device ID (e.g., LS-A0B1C2D3E4F5)
 * AP Password = smartkraft
 * mDNS = {device_id}.local
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default AP password */
#define WIFI_AP_PASSWORD "smartkraft"

/** AP channel */
#define WIFI_AP_CHANNEL 1

/** Max AP connections */
#define WIFI_AP_MAX_CONN 4

/** WiFi scan max results */
#define WIFI_SCAN_MAX_AP 20

/** Connection timeout in seconds */
#define WIFI_CONNECT_TIMEOUT_SEC 15

/** Max STA retry count */
#define WIFI_MAX_RETRY 5

/**
 * @brief WiFi connection state
 */
typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_FAILED
} wifi_state_t;

/**
 * @brief WiFi scan result entry
 */
typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_scan_result_t;

/**
 * @brief WiFi status structure
 */
typedef struct {
    bool ap_active;
    bool sta_connected;
    wifi_state_t sta_state;
    char ap_ssid[33];
    char ap_ip[16];
    char sta_ssid[33];
    char sta_ip[16];
    int8_t sta_rssi;
    uint8_t ap_clients;
} wifi_status_t;

/**
 * @brief Initialize WiFi manager
 * 
 * Starts AP mode with Device ID as SSID and "smartkraft" as password.
 * Initializes mDNS with device_id.local hostname.
 * 
 * @return ESP_OK on success
 */
int wifi_manager_init(void);

/**
 * @brief Start AP mode
 * 
 * SSID = Device ID (LS-XXXXXXXXXXXX)
 * Password = smartkraft
 * 
 * @return ESP_OK on success
 */
int wifi_manager_start_ap(void);

/**
 * @brief Stop AP mode
 * 
 * @return ESP_OK on success
 */
int wifi_manager_stop_ap(void);

/**
 * @brief Connect to WiFi network (STA mode)
 * 
 * @param ssid Network SSID
 * @param password Network password (can be NULL for open networks)
 * @return ESP_OK if connection started (check status for result)
 */
int wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Start WiFi connection asynchronously (non-blocking)
 * 
 * @param ssid Network SSID
 * @param password Network password (can be NULL for open networks)
 * @return ESP_OK if connection initiated
 */
int wifi_manager_connect_async(const char *ssid, const char *password);

/**
 * @brief Disconnect from WiFi network
 * 
 * @return ESP_OK on success
 */
int wifi_manager_disconnect(void);

/**
 * @brief Scan for available WiFi networks
 * 
 * @param results Array to store results
 * @param max_results Maximum number of results
 * @return Number of networks found, or negative error code
 */
int wifi_manager_scan(wifi_scan_result_t *results, uint16_t max_results);

/**
 * @brief Get current WiFi status
 * 
 * @param status Pointer to status structure to fill
 * @return ESP_OK on success
 */
int wifi_manager_get_status(wifi_status_t *status);

/**
 * @brief Check if STA is connected
 * 
 * @return true if connected to a network
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get STA IP address string
 * 
 * @return IP address string or "0.0.0.0" if not connected
 */
const char* wifi_manager_get_sta_ip(void);

/**
 * @brief Get AP IP address string
 * 
 * @return AP IP address (typically "192.168.4.1")
 */
const char* wifi_manager_get_ap_ip(void);

/**
 * @brief Get mDNS hostname
 * 
 * @return Hostname without .local suffix
 */
const char* wifi_manager_get_hostname(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
