/**
 * Relay Manager - GPIO18 (D10) Röle Kontrolü
 *
 * Özellikler:
 * - Normal/Inverted mod
 * - Gecikme (delay): Tetikleme öncesi bekleme
 * - Süre (duration): Otomatik kapanma
 * - Pulse modu: Periyodik açma/kapama
 * - Dahili zamanlama: Çağrı frekansından bağımsız çalışır
 *
 * Bağımlılık: Yok (sadece ESP-IDF GPIO + Timer)
 * Katman: 0 (Donanım)
 */

#ifndef RELAY_MANAGER_H
#define RELAY_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_GPIO_PIN  18  // D10

// Röle durumları
typedef enum {
    RELAY_STATE_IDLE = 0,   // Kapalı, beklemede
    RELAY_STATE_DELAY,      // Gecikme sayılıyor
    RELAY_STATE_ACTIVE,     // Açık (sürekli)
    RELAY_STATE_PULSING     // Açık (pulse modunda)
} relay_state_t;

// Röle yapılandırması
typedef struct {
    bool inverted;              // true: LOW=enerji var
    uint32_t delay_seconds;     // Tetikleme öncesi bekleme (0=yok)
    uint32_t duration_seconds;  // Açık kalma süresi (0=süresiz)
    bool pulse_enabled;         // Pulse modu aktif mi
    uint32_t pulse_on_ms;       // Pulse açık süresi
    uint32_t pulse_off_ms;      // Pulse kapalı süresi
} relay_config_t;

#define RELAY_CONFIG_DEFAULT() {        \
    .inverted = false,                  \
    .delay_seconds = 0,                 \
    .duration_seconds = 0,              \
    .pulse_enabled = false,             \
    .pulse_on_ms = 500,                 \
    .pulse_off_ms = 500                 \
}

// Durum bilgisi (read-only snapshot)
typedef struct {
    relay_state_t state;
    bool gpio_level;            // Fiziksel GPIO seviyesi
    bool energy_output;         // Mantıksal enerji çıkışı
    uint32_t remaining_delay;   // Kalan gecikme (saniye)
    uint32_t remaining_duration;// Kalan süre (saniye)
    uint32_t pulse_count;       // Toplam pulse sayısı
    uint32_t trigger_count;     // Toplam tetikleme sayısı
} relay_status_t;

/**
 * Röle yöneticisini başlat (GPIO + dahili timerlar)
 */
esp_err_t relay_manager_init(void);

/**
 * Röle yöneticisini kapat
 */
esp_err_t relay_manager_deinit(void);

/**
 * Yapılandırmayı güncelle
 */
esp_err_t relay_set_config(const relay_config_t *config);

/**
 * Mevcut yapılandırmayı al
 */
esp_err_t relay_get_config(relay_config_t *config);

/**
 * Röleyi tetikle (delay → active/pulse → duration sonunda kapat)
 */
esp_err_t relay_trigger(void);

/**
 * Röleyi hemen aç (delay atlanır)
 */
esp_err_t relay_on(void);

/**
 * Röleyi kapat
 */
esp_err_t relay_off(void);

/**
 * Toggle (açıksa kapat, kapalıysa aç)
 */
esp_err_t relay_toggle(void);

/**
 * Tek pulse gönder (blocking, ms cinsinden)
 */
esp_err_t relay_pulse(uint32_t duration_ms);

/**
 * Pulse modunu başlat/durdur
 */
esp_err_t relay_start_pulsing(void);
esp_err_t relay_stop_pulsing(void);

/**
 * Durum bilgisini al
 */
esp_err_t relay_get_status(relay_status_t *status);

/**
 * Enerji çıkışı var mı (inverted hesaba katılmış mantıksal değer)
 */
bool relay_get_energy_output(void);

/**
 * Ham GPIO seviyesi
 */
bool relay_get_gpio_level(void);

/**
 * Periyodik tick - herhangi bir frekansta çağrılabilir
 * Dahili olarak 1 saniye aralıklarla işlem yapar
 */
void relay_tick(void);

/**
 * Debug istatistikleri yazdır
 */
void relay_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // RELAY_MANAGER_H
