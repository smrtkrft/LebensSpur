#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Telegram bot yapilandirma */
typedef struct {
    char token[72];      /* Bot token */
    char chat_id[32];    /* Chat ID */
    char message[256];   /* Mesaj sablonu */
} telegram_config_t;

/* Baslatma: NVS'den kayitli ayarlari yukler */
esp_err_t telegram_client_init(void);

/* Ayarlari kaydet (NVS) */
esp_err_t telegram_client_save_config(const telegram_config_t *cfg);

/* Kayitli ayarlari oku */
esp_err_t telegram_client_get_config(telegram_config_t *cfg);

/* Ayar var mi? (token + chat_id dolu) */
bool telegram_client_has_config(void);

/* Telegram mesaji gonder (bloklayan cagri, ayri task'ta calistirin) */
esp_err_t telegram_client_send(const char *text);
