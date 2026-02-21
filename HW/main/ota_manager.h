/**
 * OTA Manager - Over-The-Air Firmware Guncelleme
 *
 * URL (HTTPS) veya harici flash dosyasindan OTA destegi.
 * esp_https_ota ile URL OTA, esp_ota_ops ile dosya OTA.
 * Rollback ve firmware onaylama mekanizmasi.
 *
 * Bagimlilik: file_manager (Katman 1), wifi_manager (Katman 3)
 * Katman: 3 (Iletisim)
 */

#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// OTA durumlari
typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_UPDATING,
    OTA_STATE_COMPLETE,
    OTA_STATE_ERROR
} ota_state_t;

// OTA ilerleme callback
typedef void (*ota_progress_cb_t)(uint32_t current, uint32_t total);

/**
 * OTA sistemini baslat (partition kontrolu, version okuma)
 */
esp_err_t ota_manager_init(void);

/**
 * URL'den OTA guncelleme baslat (blocking)
 * @param url Firmware URL'si (https://...)
 */
esp_err_t ota_manager_start_from_url(const char *url);

/**
 * Harici flash'tan OTA guncelleme (blocking)
 * @param filepath Firmware dosya yolu (orn: /ext/firmware.bin)
 */
esp_err_t ota_manager_start_from_file(const char *filepath);

/**
 * OTA durumunu al
 */
ota_state_t ota_manager_get_state(void);

/**
 * OTA ilerleme yuzdesini al (0-100)
 */
uint8_t ota_manager_get_progress(void);

/**
 * OTA iptal et (sadece state sifirlar)
 */
esp_err_t ota_manager_abort(void);

/**
 * Guncellemeden sonra yeniden baslat
 */
void ota_manager_restart(void);

/**
 * Ilerleme callback'i ayarla
 */
void ota_manager_set_progress_callback(ota_progress_cb_t callback);

/**
 * Mevcut firmware version stringi
 */
const char *ota_manager_get_current_version(void);

/**
 * Bir onceki partition'a geri don (reboot yapar)
 */
esp_err_t ota_manager_rollback(void);

/**
 * Mevcut firmware'i onayla (rollback'i devre disi birak)
 */
esp_err_t ota_manager_mark_valid(void);

/**
 * Debug bilgileri
 */
void ota_manager_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_MANAGER_H
