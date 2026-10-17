#pragma once
/**
 * LebensSpur - API istemcisi (Webhook + MQTT)
 *
 * Erken uyari (alarm) ve tetikleme aninda dis sistemlere bildirim:
 *   - Webhook: HTTP(S) istegi (GET/POST/PUT), ozel basliklar, sablonlu govde
 *   - MQTT: broker'a tek mesaj yayini (esp-mqtt), sablonlu yuk
 * Sablon yer tutuculari: {status} {time} {device}
 *   status: "alarm" | "triggered" | "test"
 *   time:   epoch saniye (SNTP senkronu varsa) yoksa calisma suresi (sn)
 *   device: cihaz adi (LS-XXXX)
 * NVS: ls_api
 */
#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    char wh_url[256];
    char wh_method[8];       /* GET | POST | PUT */
    char wh_headers[256];    /* JSON nesnesi: {"Authorization":"Bearer x"} */
    char wh_body[512];       /* sablon */
    char mq_url[128];        /* mqtt://host:1883 */
    char mq_topic[128];
    char mq_user[64];
    char mq_pass[64];
    char mq_payload[256];    /* sablon */
} api_config_t;

esp_err_t api_client_init(void);
esp_err_t api_client_get_config(api_config_t *cfg);
esp_err_t api_client_save_config(const api_config_t *cfg);
bool api_client_has_webhook(void);
bool api_client_has_mqtt(void);

/** Webhook gonder (bloklayan; ayri gorevde cagirin). 2xx -> ESP_OK */
esp_err_t api_client_send_webhook(const char *status);

/** MQTT yayinla (bloklayan; baglan-yayinla-kapat). */
esp_err_t api_client_send_mqtt(const char *status);

/** Yapilandirilmis tum kanallara gonder; sonuclari sys_log'a yazar. */
void api_client_notify_all(const char *status);
