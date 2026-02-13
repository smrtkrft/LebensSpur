/**
 * GUI Downloader - GitHub'dan Web Arayuzu Indirme
 *
 * Setup sirasinda GUI klasorundeki dosyalari GitHub raw URL'den
 * indirip harici flash'a (LittleFS) kaydeder.
 * Fastly CDN IP adresleri ile DNS bypass destegi.
 * FreeRTOS task ile arka planda calisir.
 *
 * Bagimlilik: file_manager (Katman 1), wifi_manager (Katman 3)
 * Katman: 3 (Iletisim)
 */

#ifndef GUI_DOWNLOADER_H
#define GUI_DOWNLOADER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Indirme durum enumlari
typedef enum {
    GUI_DL_STATE_IDLE,
    GUI_DL_STATE_CONNECTING,
    GUI_DL_STATE_DOWNLOADING,
    GUI_DL_STATE_INSTALLING,
    GUI_DL_STATE_COMPLETE,
    GUI_DL_STATE_ERROR
} gui_dl_state_t;

// Indirme durum bilgisi
typedef struct {
    gui_dl_state_t state;
    uint8_t  progress;              // 0-100
    char     message[64];           // Durum mesaji
    char     error[128];            // Hata mesaji (varsa)
    uint32_t bytes_downloaded;
    uint32_t total_bytes;
    uint8_t  files_downloaded;
    uint8_t  total_files;
} gui_dl_status_t;

/**
 * GUI Downloader'i baslat (mutex olusturur)
 */
esp_err_t gui_downloader_init(void);

/**
 * GitHub'dan GUI dosyalarini indirmeyi baslat (async task)
 * @param repo  Repository adi (NULL = varsayilan)
 * @param branch Branch adi (NULL = "main")
 * @param path  Klasor yolu (NULL = "GUI")
 */
esp_err_t gui_downloader_start(const char *repo, const char *branch, const char *path);

/**
 * Indirme durumunu al (thread-safe)
 */
void gui_downloader_get_status(gui_dl_status_t *status);

/**
 * Indirme islemini iptal et
 */
void gui_downloader_cancel(void);

/**
 * GUI dosyalarinin harici flash'ta mevcut olup olmadigini kontrol et
 */
bool gui_downloader_files_exist(void);

/**
 * Varsayilan repo adini al
 */
const char *gui_downloader_get_default_repo(void);

#ifdef __cplusplus
}
#endif

#endif // GUI_DOWNLOADER_H
