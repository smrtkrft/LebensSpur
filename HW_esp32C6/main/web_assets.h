/**
 * Web Assets - Gomulu HTML Sayfalari
 *
 * Firmware icine gomulu login/index/setup HTML sayfalari.
 * Harici flash'ta GUI dosyalari yoksa fallback olarak kullanilir.
 * setup.html CMakeLists.txt'de EMBED_TXTFILES ile dahil edilir.
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
 * Web dosyalarini harici flash'a yukle (ilk boot veya guncelleme)
 * @param force true ise mevcut dosyalarin uzerine yazar
 */
esp_err_t web_assets_deploy(bool force);

/**
 * Web dosyalarinin yuklu olup olmadigini kontrol et
 */
bool web_assets_installed(void);

/**
 * Web assets versiyonunu al
 */
const char *web_assets_get_version(void);

/**
 * Varsayilan web dosyalarini olustur (fallback icin)
 */
esp_err_t web_assets_create_defaults(void);

/**
 * Gomulu HTML stringlerini al
 */
const char *web_assets_get_login_html(void);
const char *web_assets_get_index_html(void);
const char *web_assets_get_setup_html(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_ASSETS_H
