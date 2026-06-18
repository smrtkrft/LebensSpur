#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * GPIO'lari yapilandir (buton + relais)
 * Buton: GPIO18 (D10), active LOW, dahili pull-up
 * Relais: GPIO19 (D8), active HIGH
 */
esp_err_t output_init(void);

/**
 * Buton basildiginda cagrilacak callback'i ata
 * callback NULL ise buton islevi devre disi
 */
void output_set_button_callback(void (*cb)(void));

/**
 * Factory reset callback'i ata (5x basma)
 */
void output_set_factory_reset_callback(void (*cb)(void));

/**
 * Relay yapilandirmasini ayarla
 * duration: toplam calisma suresi (saniye), 0 = suresiz (SUSTAINED)
 * pulse: pulse suresi (saniye), 0 = pulse kapalı (surekli acik)
 * delay: baslamadan onceki gecikme (saniye)
 */
void output_set_relay_config(uint16_t duration, uint8_t pulse, uint8_t delay);

/**
 * Relay yapilandirmasini oku
 */
void output_get_relay_config(uint16_t *duration, uint8_t *pulse, uint8_t *delay);

/**
 * Ters mantik ayarla (true = idle'da enerji VAR, tetiklenince YOK)
 */
void output_set_inverted(bool inverted);

/**
 * Ters mantik durumu
 */
bool output_get_inverted(void);

/**
 * Relais'i tetikle (yapilandirmaya gore: delay -> pulse/sustained)
 */
void output_relay_trigger(void);

/**
 * Relais'i kapat (pulse task dahil durdurur)
 */
void output_relay_off(void);

/**
 * Relais durumu
 */
bool output_relay_is_on(void);

/**
 * Yapilandirmayi NVS'den yukle
 */
esp_err_t output_load_config(void);

/**
 * Yapilandirmayi NVS'ye kaydet
 */
esp_err_t output_save_config(void);
