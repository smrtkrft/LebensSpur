#ifndef FW_OTA_H
#define FW_OTA_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * LebensSpur - Firmware OTA Guncelleme
 * GitHub Releases uzerinden firmware indirip dahili flash'a yazar
 * esp_https_ota ile guvenli A/B partition guncelleme + rollback destegi
 */

typedef enum {
    FW_OTA_IDLE,              // 0 - Bosta
    FW_OTA_CHECKING,          // 1 - Versiyon kontrol ediliyor
    FW_OTA_UPDATE_AVAILABLE,  // 2 - Guncelleme mevcut
    FW_OTA_NO_UPDATE,         // 3 - Guncel
    FW_OTA_DOWNLOADING,       // 4 - Indiriliyor + yaziliyor
    FW_OTA_DONE,              // 5 - Tamamlandi (restart gerekli)
    FW_OTA_ERROR              // 6 - Hata
} fw_ota_state_t;

typedef struct {
    fw_ota_state_t state;
    int progress_pct;
    char message[128];
    char remote_version[16];
    char current_version[16];
} fw_ota_status_t;

/**
 * fw_ota_init - Baslangic (mutex olustur)
 */
esp_err_t fw_ota_init(void);

/**
 * fw_ota_check - Versiyon kontrol (arka plan task)
 * @param custom_url  NULL ise resmi GitHub Releases, doluysa custom binary URL
 */
esp_err_t fw_ota_check(const char *custom_url);

/**
 * fw_ota_start - Firmware indir ve yaz (sadece UPDATE_AVAILABLE durumunda)
 */
esp_err_t fw_ota_start(void);

/**
 * fw_ota_get_status - Mevcut durumu oku (thread-safe)
 */
void fw_ota_get_status(fw_ota_status_t *status);

/**
 * fw_ota_rollback - Onceki partition'a don (reboot gerekmez, kullanici yapar)
 */
esp_err_t fw_ota_rollback(void);

/**
 * fw_ota_is_busy - Task calisiyor mu?
 */
bool fw_ota_is_busy(void);

/**
 * fw_ota_can_rollback - Diger partition'da gecerli firmware var mi?
 */
bool fw_ota_can_rollback(void);

/**
 * fw_ota_get_running_label - Calistigi partition etiketi ("ota_0" veya "ota_1")
 */
const char *fw_ota_get_running_label(void);

/**
 * fw_ota_get_rollback_version - Diger partition'daki firmware versiyonu
 * @return  Versiyon string veya NULL (gecerli firmware yoksa)
 */
const char *fw_ota_get_rollback_version(void);

#endif
