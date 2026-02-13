/**
 * Web Assets - Gomulu Setup Sayfasi
 *
 * Sadece setup.html firmware icinde gomulu.
 * GUI dosyalari (index.html, login vb.) setup sirasinda
 * GitHub'dan indirilir ve LittleFS'ten servis edilir.
 */

#include "web_assets.h"
#include "file_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WEB_ASSETS";

#define VERSION_FILE FILE_MGR_WEB_PATH "/version.txt"

// Embedded setup.html (CMakeLists.txt'de EMBED_TXTFILES ile dahil)
extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[] asm("_binary_setup_html_end");

// ============================================================================
// Getter fonksiyonlari
// ============================================================================

const char *web_assets_get_setup_html(void)
{
    return (const char *)setup_html_start;
}

bool web_assets_installed(void)
{
    return file_manager_exists(VERSION_FILE);
}
