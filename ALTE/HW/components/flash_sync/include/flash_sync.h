#pragma once

#include "esp_err.h"

/**
 * Flash senkronizasyon modulu baslatma
 * Dahili config partition ile harici flash arasinda senkronizasyon saglar
 */
esp_err_t flash_sync_init(void);

/**
 * Konfigurasyon verisini NVS'ye kaydet
 */
esp_err_t flash_sync_save_config(const char *key, const char *value);

/**
 * Konfigurasyon verisini NVS'den oku
 * buf_size: buffer boyutu (giris/cikis)
 */
esp_err_t flash_sync_load_config(const char *key, char *value, size_t *buf_size);

/**
 * Harici flash'a web icerigini senkronize et
 * Dahili config'den harici flash'a gerekli dosyalari kopyalar
 */
esp_err_t flash_sync_to_external(void);
