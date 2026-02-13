/**
 * WiFi Manager - AP + STA Yönetimi
 *
 * APSTA modu: AP her zaman aktif (config'e bağlı), STA bağlantı dener.
 * AP SSID = device_id (LS-XXXXXXXXXX), benzersiz cihaz kimliği.
 * Primary WiFi başarısız olursa secondary'ye geçer.
 * Config'den SSID/password, static IP, mDNS yüklenir.
 *
 * Bağımlılık: config_manager (Katman 2), device_id (Katman 0)
 * Katman: 3 (İletişim)
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// AP sabitleri (SSID runtime'da device_id'den oluşturulur)
#define WIFI_AP_PASS            "smartkraft"
#define WIFI_AP_CHANNEL         1
#define WIFI_AP_MAX_CONN        4
#define WIFI_MAX_SCAN_RESULTS   20

// Bağlantı callback
typedef void (*wifi_event_cb_t)(bool connected);

/**
 * WiFi başlat (AP + STA modu)
 * AP SSID = device_id (LS-XXXXXXXXXX)
 * Config'den STA ayarlarını yükler, otomatik bağlanır
 */
esp_err_t wifi_manager_init(void);

/**
 * Belirtilen ağa bağlan (runtime)
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * Bağlantıyı kes
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * STA bağlı mı
 */
bool wifi_manager_is_connected(void);

/**
 * STA IP adresi (bağlı değilse "0.0.0.0")
 */
const char *wifi_manager_get_ip(void);

/**
 * AP IP adresi
 */
const char *wifi_manager_get_ap_ip(void);

/**
 * AP SSID (= device_id)
 */
const char *wifi_manager_get_ap_ssid(void);

/**
 * Bağlantı callback kaydet
 */
void wifi_manager_set_callback(wifi_event_cb_t callback);

/**
 * WiFi tarama (blocking)
 */
esp_err_t wifi_manager_scan(void);

/**
 * Tarama sonuçlarını al
 */
esp_err_t wifi_manager_scan_get_results(wifi_ap_record_t *records, uint16_t *count);

/**
 * Bulunan AP sayısı
 */
uint16_t wifi_manager_scan_get_count(void);

/**
 * Config'deki birincil/ikincil ağa bağlanmayı dene
 */
esp_err_t wifi_manager_connect_from_config(void);

/**
 * Debug bilgileri
 */
void wifi_manager_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
