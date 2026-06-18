#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     enabled;
    int32_t  remaining_seconds;
    int32_t  total_seconds;
    bool     vacation_enabled;
    int32_t  vacation_remaining;
    int32_t  vacation_total;
    uint8_t  alarm_count;
} timer_state_t;

/**
 * Timer manager baslatma - NVS'den mevcut durumu yukler
 */
esp_err_t timer_manager_init(void);

/**
 * Yeni zamanlayici ayarla
 * unit: "gun", "saat", veya "dakika"
 * value: 1-60 arasi
 */
esp_err_t timer_manager_set(const char *unit, int value);

/**
 * Mevcut zamanlayici durumunu getir
 */
timer_state_t timer_manager_get_state(void);

/**
 * Zamanlayiciyi iptal et
 */
esp_err_t timer_manager_cancel(void);

/**
 * Zamanlayiciyi bastan baslat (ayni sure ile)
 */
esp_err_t timer_manager_restart(void);

/**
 * Tatil modunu baslat (1-60 gun arasi)
 * Ana timer sifirlanir ve tatil bitene kadar bekler
 */
esp_err_t timer_manager_set_vacation(int days);

/**
 * Tatil modunu iptal et
 * Ana timer otomatik baslatilir
 */
esp_err_t timer_manager_cancel_vacation(void);

/**
 * Alarm sayisini ayarla (0-10 arasi)
 * Tetiklemeden once kac uyari gonderilecegini belirler
 */
esp_err_t timer_manager_set_alarm_count(int count);

/**
 * Timer suresi dolunca cagrilacak callback'i ata
 * Callback esp_timer context'inde cagrilir - kisa tutun
 */
void timer_manager_set_expiry_callback(void (*cb)(void));

/**
 * Alarm tetiklenince cagrilacak callback'i ata
 * alarm_index: 0-based, kacinci alarm tetiklendi
 * Callback esp_timer context'inde cagrilir - kisa tutun
 */
void timer_manager_set_alarm_callback(void (*cb)(int alarm_index));

/**
 * Her N saniyede cagrilacak tick callback (SSE sync icin)
 * Callback esp_timer context'inde cagrilir - kisa tutun
 */
void timer_manager_set_tick_callback(void (*cb)(void));
