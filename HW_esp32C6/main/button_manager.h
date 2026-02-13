/**
 * Button Manager - GPIO17 (D7) Fiziksel Buton Kontrolü
 *
 * Özellikler:
 * - Debounce (50ms)
 * - Kısa basma (<1s), uzun basma (1-3s), çok uzun basma (>3s) algılama
 * - Callback tabanlı olay bildirimi
 * - Polling tabanlı (10ms tick ile çağrılmalı)
 *
 * Bağımlılık: Yok (sadece ESP-IDF GPIO + Timer)
 * Katman: 0 (Donanım)
 */

#ifndef BUTTON_MANAGER_H
#define BUTTON_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUTTON_GPIO_PIN     17  // D7 (Active LOW, dahili pull-up)

// Buton olayları
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESS,         // Kısa basma (< 1s)
    BUTTON_EVENT_LONG_PRESS,    // Uzun basma (1-3s)
    BUTTON_EVENT_VERY_LONG,     // Çok uzun basma (> 3s)
    BUTTON_EVENT_RELEASE        // Bırakma
} button_event_t;

// Olay callback tipi
typedef void (*button_callback_t)(button_event_t event);

/**
 * Buton yöneticisini başlat (GPIO yapılandırması)
 */
esp_err_t button_manager_init(void);

/**
 * Buton yöneticisini kapat
 */
esp_err_t button_manager_deinit(void);

/**
 * Olay callback'i kaydet
 */
void button_set_callback(button_callback_t callback);

/**
 * Buton şu an basılı mı (debounce sonrası stabil durum)
 */
bool button_is_pressed(void);

/**
 * Basılı kalma süresi (ms), basılı değilse 0
 */
uint32_t button_get_press_duration(void);

/**
 * Periyodik tick - ana döngüden ~10ms aralıkla çağrılmalı
 * Debounce ve olay algılama yapar
 */
void button_tick(void);

/**
 * Debug istatistikleri yazdır
 */
void button_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_MANAGER_H
