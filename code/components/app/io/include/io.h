/*
 * io — fiziksel düğme (GPIO18, aktif LOW, iç pull-up) ve röle (GPIO19, aktif HIGH; inverted ise tersi).
 *
 * Röle pini kuralları (kullanıcı şartı: açılışta/yeniden başlatmada röle milisaniye bile oynamaz):
 *  - Polarite (inverted) bilinmeden pin SÜRÜLMEZ: açılışta pad ROM'un bıraktığı halde kalır.
 *  - Ayarlar okununca önce çıkış yazmacına boşta seviyesi yazılır, sonra çıkış açılır (tek, doğru kenar).
 *  - Her bilinçli yeniden başlatmada (esp_restart: OTA, yeniden başlat, fabrika ayarı, geri yükleme)
 *    kapanış işleyicisi boşta seviyesini yazar ve pad'i TUTAR (hold): reset + bootloader boyunca pin
 *    boşta kalır; açılışta aynı seviye sürüldükten sonra tutma bırakılır.
 *  - Güç kesilip gelince ROM'dan app_main'e kadar (~300 ms) pad yazılımın elinde değildir: boşta seviyesini
 *    yalnız donanımdaki direnç tutar (aktif-yüksek: sürücü girişinde pull-down; inverted: pull-up).
 *  - gpio_reset_pin KULLANILMAZ (pull-up açar).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*io_button_isr_cb_t)(void);       /* ISR bağlamında; yalnız kuyruğa olay bırakmalı */

void      io_relay_early_init(void);             /* app_main ilk satırı: pin'e dokunmaz, açılış halini kaydeder */
void      io_relay_init(bool inverted);          /* ayarlar okununca: boşta seviyesi → çıkış → tutmayı bırak */
void      io_relay_set(bool energized, bool inverted);
uint32_t  io_relay_boot_pad(void);               /* açılıştaki IO_MUX değeri (teşhis) */
bool      io_relay_boot_held(void);              /* açılışta önceki yeniden başlatmanın tutması vardı */
esp_err_t io_button_init(io_button_isr_cb_t cb); /* 200 ms debounce ISR içinde */
