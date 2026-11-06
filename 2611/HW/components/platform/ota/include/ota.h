/*
 * ota — yazılım güncelleme (docs/03-arastirma/03-ota-coredump-bolum.md, 04-plan-son.md §2.6).
 *
 * Resmi kaynak: GitHub API'siz. HEAD /releases/latest → 302 Location → tag. İndirme doğrudan
 * /releases/download/<tag>/lebensspur.bin. Özel kaynak: yalnız https, doğrudan .bin adresi.
 * Kapılar (indirme başında, flash'a yazmadan): project_name aynı ve sürüm çalışandan yeni.
 * Onay: yeni imaj PENDING_VERIFY açılır; sys_worker'da koşul (≥30 s çalıştı ve WiFi bağlı ya da ≥120 s)
 * sağlanınca geçerli işaretlenir; öncesinde her reset → bootloader eski imaja döner.
 * Uzun işler (denetim, indirme) sys_worker'da; ilerleme jobs ile. Tetikleme anında ota_abort().
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    OTA_IDLE, OTA_CHECKING, OTA_AVAILABLE, OTA_NO_UPDATE, OTA_DOWNLOADING, OTA_READY, OTA_ERROR, OTA_ABORTED,
    OTA_ROLLBACK_READY,
} ota_state_t;

typedef struct {
    ota_state_t state;
    int pct;
    char message[96];
    char current[32], remote[32], rollback[32], last_invalid[32];
    bool rollback_available, pending_verify;
} ota_status_t;

esp_err_t   ota_init(void);
void        ota_get_status(ota_status_t *out);
const char *ota_state_name(ota_state_t s);
/* url NULL/"" = resmi kaynak. job: jobs kimliği (ilerleme/sonuç). Meşgulse ESP_ERR_INVALID_STATE. */
esp_err_t   ota_check(const char *url, uint16_t job);
esp_err_t   ota_start(const char *url, uint16_t job);
void        ota_abort(void);                        /* her görevden; indirme döngüsü bir sonraki parçada durur */
esp_err_t   ota_rollback(void);                     /* önceki imaj geçerliyse sonraki açılış ona */
bool        ota_restart_pending(void);              /* READY / ROLLBACK_READY: yeniden başlatma nedeni RST_OTA */
