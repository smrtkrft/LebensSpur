/*
 * net_wifi — olay güdümlü WiFi durum makinesi (03-arastirma/05). Kendi görevi yok; tüm geçişler
 * IDF varsayılan olay döngüsünde. API çağrıları yalnız olay bırakır.
 * Korunan ALTE davranışları: AP SSID = cihaz adı, 192.168.4.1, 4 istemci, APSTA; birincil → yedek;
 * 5 deneme (1..5 sn); statik IP türetme; mDNS; SNTP yalnız STA'da; TX yumuşak başlangıç + uyarlanabilir.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { WST_AP_ONLY, WST_SCANNING, WST_CONNECTING, WST_CONNECTED, WST_DISCONNECTING, WST_RETRY_WAIT } wifi_state_t;
typedef enum { WERR_NONE, WERR_AUTH, WERR_NOT_FOUND, WERR_SECURITY, WERR_OTHER } wifi_err_class_t;

typedef struct {
    wifi_state_t state;
    char ip[16];
    char ssid[33];
    int8_t rssi;
    uint8_t ap_clients;
    uint8_t tx_q;                    /* 0,25 dBm birimi (esp_wifi_set_max_tx_power) */
    bool ap_on, ap_auto_opened, using_backup;
    struct { wifi_err_class_t cls; uint8_t reason; int8_t rssi; } last_error;
} wifi_status_t;

typedef void (*net_wifi_link_cb_t)(bool up);

const char *net_wifi_device_name(void);              /* "LS-XXXXXXXXXX" (MAC'ten, ALTE ile aynı) */
esp_err_t   net_wifi_start(net_wifi_link_cb_t cb);   /* cfg: device, wifi_p, wifi_b, wifi_tx */
void        net_wifi_get_status(wifi_status_t *out);
esp_err_t   net_wifi_reconnect(void);                /* kimlik değişti: kendi kopar + baştan */
esp_err_t   net_wifi_apply_ap(void);                 /* cfg device.ap_enabled; STA bağlı değilken kapatma reddi */
esp_err_t   net_wifi_apply_tx(void);                 /* cfg wifi_tx */
void        net_wifi_set_tx_cap_q(uint8_t q);        /* brownout açılışı: geçici tavan (0 = kaldır) */
const char *net_wifi_err_class_name(wifi_err_class_t c);
