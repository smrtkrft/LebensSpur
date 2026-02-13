/**
 * Timer Scheduler - LebensSpur Dead Man's Switch
 *
 * Ana timer mantigi:
 * - Kullanici belirli araliklarda (orn: 24 saat) "hayattayim" sinyali gonderir
 * - Gondermezse uyari maili gonderilir
 * - Hala gondermezse alarm tetiklenir (mail + role)
 *
 * Relay kontrolu relay_manager uzerinden yapilir (Katman 0).
 * Config timer_config_t ve timer_runtime_t (Katman 2).
 * FreeRTOS software timer ile 1 saniyelik tick.
 *
 * Bagimlilik: config_manager (Katman 2), relay_manager (Katman 0),
 *             mail_sender (Katman 3), time_manager (Katman 1)
 * Katman: 4 (Uygulama)
 */

#ifndef TIMER_SCHEDULER_H
#define TIMER_SCHEDULER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Timer durumlari
typedef enum {
    TIMER_STATE_DISABLED = 0,   // Timer devre disi
    TIMER_STATE_ACTIVE,         // Normal calisiyor
    TIMER_STATE_WARNING,        // Uyari suresi basladi
    TIMER_STATE_TRIGGERED,      // Alarm tetiklendi
    TIMER_STATE_PAUSED          // Gecici duraklatildi
} timer_state_t;

// Timer durum bilgisi
typedef struct {
    timer_state_t state;
    uint32_t remaining_seconds;     // Kalan sure (saniye)
    uint32_t warning_seconds;       // Uyariya kalan sure
    int64_t last_reset_time;        // Son sifirlama (epoch ms)
    int64_t next_deadline;          // Sonraki deadline (epoch ms)
    uint32_t reset_count;           // Toplam sifirlama sayisi
    uint32_t warning_count;         // Uyari gonderme sayisi
    uint32_t trigger_count;         // Tetiklenme sayisi
    bool in_active_hours;           // Aktif saatler icinde mi
} timer_status_t;

// Callback fonksiyonlari
typedef void (*timer_warning_cb_t)(uint32_t remaining_minutes);
typedef void (*timer_trigger_cb_t)(void);
typedef void (*timer_reset_cb_t)(void);

/**
 * Timer scheduler'i baslat (config'den ayarlari yukler)
 */
esp_err_t timer_scheduler_init(void);

/**
 * Timer scheduler'i durdur
 */
esp_err_t timer_scheduler_deinit(void);

/**
 * Timer'i sifirla (kullanici "hayattayim" sinyali)
 */
esp_err_t timer_reset(void);

/**
 * Timer'i etkinlestir/devre disi birak
 */
esp_err_t timer_set_enabled(bool enabled);

/**
 * Timer'i gecici olarak duraklat
 */
esp_err_t timer_pause(void);

/**
 * Duraklatilmis timer'i devam ettir
 */
esp_err_t timer_resume(void);

/**
 * Guncel timer durumunu al
 */
esp_err_t timer_get_status(timer_status_t *status);

/**
 * 1 saniyelik tick (FreeRTOS timer callback)
 */
void timer_tick(void);

/**
 * Aktif saatler icinde mi
 */
bool timer_is_in_active_hours(void);

/**
 * Callback'leri ayarla
 */
void timer_set_warning_callback(timer_warning_cb_t cb);
void timer_set_trigger_callback(timer_trigger_cb_t cb);
void timer_set_reset_callback(timer_reset_cb_t cb);

/**
 * Debug bilgileri
 */
void timer_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // TIMER_SCHEDULER_H
