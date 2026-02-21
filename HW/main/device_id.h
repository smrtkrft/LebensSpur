/**
 * Device ID - ESP32-C6 Benzersiz Cihaz Kimliği
 *
 * MAC adresinden base36 formatında benzersiz ID üretir.
 * Format: LS-XXXXXXXXXX (3 prefix + 10 karakter base36)
 *
 * Bağımlılık: Yok (sadece ESP-IDF)
 * Katman: 0 (Donanım)
 */

#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_ID_LENGTH    14      // "LS-" + 10 chars + null
#define DEVICE_ID_PREFIX    "LS-"

/**
 * Device ID sistemini başlat - MAC adresini okur ve ID üretir
 */
esp_err_t device_id_init(void);

/**
 * Cihaz ID stringini al (statik buffer, kopyalamaya gerek yok)
 * Init çağrılmadıysa "LS-UNKNOWN" döner
 */
const char *device_id_get(void);

/**
 * Cihaz ID'sini verilen buffer'a kopyala
 * @param buffer Hedef (en az DEVICE_ID_LENGTH byte)
 * @param len    Buffer boyutu
 */
esp_err_t device_id_get_str(char *buffer, size_t len);

/**
 * Ham MAC adresini al (6 byte)
 */
esp_err_t device_id_get_mac(uint8_t *mac);

/**
 * Debug bilgilerini konsola yazdır
 */
void device_id_print_info(void);

#ifdef __cplusplus
}
#endif

#endif // DEVICE_ID_H
