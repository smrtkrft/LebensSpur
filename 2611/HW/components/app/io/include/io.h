/*
 * io — fiziksel düğme (GPIO18, aktif LOW, iç pull-up) ve röle (GPIO19, aktif HIGH; inverted ise tersi).
 * Röle pini açılışın ilk satırında sürülür (NVS'ten önce); gpio_reset_pin KULLANILMAZ (pull-up açar).
 * GPIO19 ROM/bootloader boyunca yüksek empedanstır: inverted bağlantıda donanım pull direnci gerekir.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef void (*io_button_isr_cb_t)(void);       /* ISR bağlamında; yalnız kuyruğa olay bırakmalı */

void      io_relay_early_init(void);             /* app_main ilk satırı: çıkış, varsayılan boşta (LOW) */
void      io_relay_set(bool energized, bool inverted);
esp_err_t io_button_init(io_button_isr_cb_t cb); /* 200 ms debounce ISR içinde */
