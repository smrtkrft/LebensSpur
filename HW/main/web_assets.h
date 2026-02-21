/**
 * Web Assets - Gomulu Setup Sayfasi
 *
 * Sadece setup.html firmware icinde gomulu.
 * GUI dosyalari (index.html, login vb.) setup sirasinda
 * GitHub'dan indirilir ve LittleFS'ten servis edilir.
 *
 * Bagimlilik: file_manager (Katman 1)
 * Katman: 4 (Uygulama)
 */

#ifndef WEB_ASSETS_H
#define WEB_ASSETS_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GUI dosyalarinin yuklu olup olmadigini kontrol et
 */
bool web_assets_installed(void);

/**
 * Gomulu setup.html sayfasini al
 */
const char *web_assets_get_setup_html(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_ASSETS_H
