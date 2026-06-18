#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SK_WIFI_SSID_MAX  32
#define SK_WIFI_PASS_MAX  64
#define SK_WIFI_IPCFG_MAX 32   // "192.168.1.50/24"

typedef struct {
    char ssid[SK_WIFI_SSID_MAX + 1];
    char password[SK_WIFI_PASS_MAX + 1];
    char static_ip[SK_WIFI_IPCFG_MAX + 1];  // empty means DHCP
} sk_wifi_cred_t;

typedef struct {
    bool connected;
    char ssid[SK_WIFI_SSID_MAX + 1];
    char ip[16];
    int  rssi;
    bool static_ip;
} sk_wifi_status_t;

// Initialize WiFi STA (no SoftAP). Reads primary credentials from NVS and
// auto-connects on boot. Publishes wifi.state events.
esp_err_t sk_wifi_init(void);

esp_err_t sk_wifi_connect_sta(const sk_wifi_cred_t *cred);
esp_err_t sk_wifi_disconnect(void);
esp_err_t sk_wifi_save_primary(const sk_wifi_cred_t *cred);
esp_err_t sk_wifi_save_backup(const sk_wifi_cred_t *cred);
esp_err_t sk_wifi_clear_primary(void);
esp_err_t sk_wifi_clear_backup(void);
bool      sk_wifi_has_primary(void);
bool      sk_wifi_has_backup(void);
esp_err_t sk_wifi_auto_connect(void);
void      sk_wifi_status(sk_wifi_status_t *out);

// Blocking scan (returns up to `max` networks).
typedef struct {
    char ssid[SK_WIFI_SSID_MAX + 1];
    int  rssi;
    int  auth;   // wifi_auth_mode_t cast to int
} sk_wifi_scan_entry_t;

int sk_wifi_scan(sk_wifi_scan_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
