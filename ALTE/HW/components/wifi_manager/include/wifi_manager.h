#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define WIFI_AP_CHANNEL     0   // Auto - APSTA modda STA kanalina uyum saglar
#define WIFI_AP_MAX_CONN    4

/**
 * WiFi manager baslatma - cihaz adini alir (LS-XXXX)
 * AP modda baslar, sifresiz, SSID = device_name
 */
esp_err_t wifi_manager_init(const char *device_name);

/**
 * AP modunu baslat
 */
esp_err_t wifi_manager_start_ap(void);

/**
 * STA moduna gec (belirtilen SSID/pass ile)
 * static_ip bos ise DHCP kullanilir, doluysa statik IP ayarlanir
 */
esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password,
                                    const char *static_ip);

/**
 * Kayitli WiFi bilgisi var mi?
 */
bool wifi_manager_has_saved_config(void);

/**
 * Mevcut WiFi durumunu getir
 * true = STA bagli, false = AP modunda
 */
bool wifi_manager_is_connected(void);

/**
 * IP adresini string olarak getir
 */
const char *wifi_manager_get_ip(void);

/**
 * Cihaz adini getir (LS-XXXX)
 */
const char *wifi_manager_get_device_name(void);

/**
 * AP SSID getir
 */
const char *wifi_manager_get_ap_ssid(void);

/**
 * Kayitli STA SSID getir (bos string donebilir)
 */
const char *wifi_manager_get_sta_ssid(void);

/**
 * Yedek WiFi bilgilerini kaydet (NVS)
 * static_ip bos ise DHCP kullanilir
 */
esp_err_t wifi_manager_save_backup(const char *ssid, const char *password,
                                    const char *static_ip);

/**
 * Kayitli yedek WiFi bilgisi var mi?
 */
bool wifi_manager_has_backup_config(void);

/**
 * Kayitli yedek WiFi SSID getir (bos string donebilir)
 */
const char *wifi_manager_get_backup_ssid(void);

/**
 * Kayitli WiFi bilgileriyle otomatik baglan (boot icin)
 * Once primary, sonra backup dener
 */
esp_err_t wifi_manager_auto_connect(void);

/**
 * AP modunu ac/kapat
 * STA bagli degilse kapatmaya izin vermez (guvenlik)
 * NVS'ye kaydeder, reboot sonrasi da gecerli
 */
esp_err_t wifi_manager_set_ap_enabled(bool enabled);

/**
 * AP modu aktif mi?
 */
bool wifi_manager_is_ap_enabled(void);

/**
 * Primary WiFi ayarlarini sil (NVS + hafiza)
 */
esp_err_t wifi_manager_clear_primary(void);

/**
 * Backup WiFi ayarlarini sil (NVS + hafiza)
 */
esp_err_t wifi_manager_clear_backup(void);
