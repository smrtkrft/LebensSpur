/**
 * GUI Slot Manager - A/B GUI Versiyon Yonetimi
 *
 * Harici flash'ta iki GUI slotu (web_a, web_b) yonetir.
 * Aktif slot degistirme, rollback, health check.
 *
 * Metadata: /ext/gui.json
 * Slot A:   /ext/web_a/
 * Slot B:   /ext/web_b/
 *
 * Bagimlilik: file_manager (Katman 1)
 * Katman: 2
 */

#ifndef GUI_SLOT_H
#define GUI_SLOT_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Slot tanimlari
#define GUI_SLOT_A_PATH     "/ext/web_a"
#define GUI_SLOT_B_PATH     "/ext/web_b"
#define GUI_META_FILE       "/ext/gui.json"

// Health check: kac basarisiz boot sonrasi rollback
#define GUI_HEALTH_MAX_FAILS    3

typedef enum {
    GUI_SLOT_A = 0,
    GUI_SLOT_B = 1,
} gui_slot_id_t;

typedef struct {
    gui_slot_id_t active;           // Aktif slot (a veya b)
    char ver_a[16];                 // Slot A versiyon
    char ver_b[16];                 // Slot B versiyon
    int boot_count;                 // Basarisiz boot sayaci
    bool health_confirmed;          // GUI health ping alindi mi
} gui_slot_meta_t;

/**
 * Slot manager'i baslat (gui.json oku)
 * file_manager_init() sonrasi cagrilmali
 */
esp_err_t gui_slot_init(void);

/**
 * Aktif slot'un web path'ini dondur
 * Ornek: "/ext/web_a" veya "/ext/web_b"
 */
const char *gui_slot_get_active_path(void);

/**
 * Inaktif slot'un web path'ini dondur (download hedefi)
 */
const char *gui_slot_get_inactive_path(void);

/**
 * Aktif slot ID
 */
gui_slot_id_t gui_slot_get_active(void);

/**
 * Aktif slot versiyonu
 */
const char *gui_slot_get_active_version(void);

/**
 * Yedek slot versiyonu (bossa "" doner)
 */
const char *gui_slot_get_backup_version(void);

/**
 * Aktif slot'u degistir (indirme tamamlandiktan sonra)
 * Inaktif slot aktif olur, eski aktif yedek olur.
 * @param new_version Yeni versiyon string
 */
esp_err_t gui_slot_switch(const char *new_version);

/**
 * Rollback: yedek slot'a geri don
 * Yedek slot aktif olur.
 */
esp_err_t gui_slot_rollback(void);

/**
 * Health check: GUI basariyla yuklendi
 * boot_count sifirlanir
 */
esp_err_t gui_slot_health_ok(void);

/**
 * Boot sayacini artir, gerekirse otomatik rollback yap
 * web_server_start oncesi cagrilir
 * @return true: rollback yapildi
 */
bool gui_slot_check_health(void);

/**
 * GUI slotlari mevcut mu (en az aktif slot'ta index.html var mi)
 */
bool gui_slot_has_gui(void);

/**
 * Metadata bilgilerini al (JSON API icin)
 */
const gui_slot_meta_t *gui_slot_get_meta(void);

#ifdef __cplusplus
}
#endif

#endif // GUI_SLOT_H
