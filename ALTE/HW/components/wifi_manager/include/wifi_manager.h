#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define WIFI_AP_CHANNEL     0   // Auto - APSTA modda STA kanalina uyum saglar
#define WIFI_AP_MAX_CONN    4

/* TX gucu sinirlari (dBm). Pik akim TX gucuyle dogrudan iliskilidir. */
#define WIFI_TX_DBM_MIN         8
#define WIFI_TX_DBM_MAX         20
#define WIFI_TX_DBM_DEFAULT     17  // Varsayilan tavan (20 yerine; pik akimi dusurur)
#define WIFI_TX_DBM_BOOT        8   // Acilista yumusak baslangic

/* Statik IP ayari: ip bos ise DHCP. gw/mask/dns bos ise varsayilan turetilir
 * (gw = x.x.x.1, mask = 255.255.255.0, dns = gw + 8.8.8.8). */
typedef struct {
    char ip[16];
    char gw[16];
    char mask[16];
    char dns[16];
} wifi_static_cfg_t;

/**
 * WiFi manager baslatma - cihaz adini alir (LS-XXXX)
 * AP modda baslar, sifresiz, SSID = device_name
 */
esp_err_t wifi_manager_init(const char *device_name);

/** AP modunu baslat */
esp_err_t wifi_manager_start_ap(void);

/**
 * STA moduna gec (belirtilen SSID/pass ile) ve NVS'ye kaydet.
 * st NULL veya st->ip bos ise DHCP kullanilir.
 */
esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password,
                                    const wifi_static_cfg_t *st);

/** Kayitli WiFi bilgisi var mi? */
bool wifi_manager_has_saved_config(void);

/** true = STA bagli, false = AP modunda */
bool wifi_manager_is_connected(void);

/** IP adresini string olarak getir */
const char *wifi_manager_get_ip(void);

/** Cihaz adini getir (LS-XXXX) */
const char *wifi_manager_get_device_name(void);

/** AP SSID getir */
const char *wifi_manager_get_ap_ssid(void);

/** Kayitli STA SSID getir (bos string donebilir) */
const char *wifi_manager_get_sta_ssid(void);

/** Kayitli primary statik IP ayari (GUI on-doldurma icin) */
const wifi_static_cfg_t *wifi_manager_get_primary_static(void);

/** Yedek WiFi bilgilerini kaydet (NVS). st NULL/ip bos ise DHCP */
esp_err_t wifi_manager_save_backup(const char *ssid, const char *password,
                                    const wifi_static_cfg_t *st);

/** Kayitli yedek WiFi bilgisi var mi? */
bool wifi_manager_has_backup_config(void);

/** Kayitli yedek WiFi SSID getir (bos string donebilir) */
const char *wifi_manager_get_backup_ssid(void);

/** Kayitli backup statik IP ayari */
const wifi_static_cfg_t *wifi_manager_get_backup_static(void);

/** Kayitli WiFi bilgileriyle otomatik baglan (boot icin). Once primary, sonra backup */
esp_err_t wifi_manager_auto_connect(void);

/**
 * AP modunu ac/kapat. STA bagli degilse kapatmaya izin vermez (guvenlik).
 * NVS'ye kaydeder, reboot sonrasi da gecerli
 */
esp_err_t wifi_manager_set_ap_enabled(bool enabled);

/** AP modu aktif mi? */
bool wifi_manager_is_ap_enabled(void);

/** Primary WiFi ayarlarini sil (NVS + hafiza) */
esp_err_t wifi_manager_clear_primary(void);

/** Backup WiFi ayarlarini sil (NVS + hafiza) */
esp_err_t wifi_manager_clear_backup(void);

/**
 * TX gucu ayari. max_dbm: tavan (8..20). adaptive: RSSI'ye gore otomatik dusurme.
 * NVS'ye kaydedilir, hemen uygulanir.
 */
esp_err_t wifi_manager_set_tx_power(int max_dbm, bool adaptive);

/** Ayarli tavan (dBm) */
int wifi_manager_get_tx_power_max(void);

/** Su an uygulanan TX gucu (dBm) */
int wifi_manager_get_tx_power_now(void);

/** Uyarlanabilir mod acik mi */
bool wifi_manager_get_tx_adaptive(void);

/** AP'ye bagli istemci sayisi */
int wifi_manager_get_ap_client_count(void);

/** STA linki RSSI (bagli degilse 0) */
int wifi_manager_get_sta_rssi(void);
