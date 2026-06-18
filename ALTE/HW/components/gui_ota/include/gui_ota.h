#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    GUI_OTA_IDLE,
    GUI_OTA_CHECKING,
    GUI_OTA_DOWNLOADING,
    GUI_OTA_VERIFYING,
    GUI_OTA_SWITCHING,
    GUI_OTA_DONE,
    GUI_OTA_ERROR,
    GUI_OTA_NO_UPDATE
} gui_ota_state_t;

typedef struct {
    gui_ota_state_t state;
    int file_index;
    int file_count;
    int progress_pct;
    char message[128];
    char remote_version[16];
    char current_version[16];
} gui_ota_status_t;

typedef struct {
    bool use_custom_source;
    char repo_url[128];
    char branch[32];
} gui_ota_params_t;

esp_err_t gui_ota_init(void);
esp_err_t gui_ota_start(const gui_ota_params_t *params);
void gui_ota_get_status(gui_ota_status_t *status);
esp_err_t gui_ota_rollback(void);
bool gui_ota_is_busy(void);
