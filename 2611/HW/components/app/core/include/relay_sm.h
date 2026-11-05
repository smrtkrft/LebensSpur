/*
 * relay_sm — röle çalışma sırası: gecikme → sürekli ya da darbeli → süre sonunda kapalı (saf C).
 * Görev ve vTaskDelay yok: app_core her değerlendirmede now ile ilerletir; geç gelen değerlendirme
 * fazları kaçırmaz (fazlar kendi son tarihlerine bağlıdır). Ters mantık (inverted) io katmanında uygulanır.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { RS_OFF, RS_DELAY, RS_ON, RS_PULSE_ON, RS_PULSE_OFF } relay_state_t;

typedef struct {
    uint8_t  delay_s;     /* 0..60 */
    uint8_t  pulse_s;     /* 0 = sürekli; 1..60 darbe açık/kapalı süresi */
    uint16_t duration_s;  /* 0 = sınırsız (ertelemeye kadar); 1..3600 */
} relay_cfg_t;

typedef struct {
    relay_state_t state;
    relay_cfg_t cfg;
    uint16_t dur_s;
    int64_t phase_deadline_us;
    int64_t end_deadline_us;   /* INT64_MAX = sınırsız */
} relay_sm_t;

void     relay_sm_init(relay_sm_t *r, const relay_cfg_t *c);
void     relay_sm_set_cfg(relay_sm_t *r, const relay_cfg_t *c);   /* çalışan sırayı etkilemez */
void     relay_sm_trigger(relay_sm_t *r, int64_t now_us, uint16_t duration_override_s);  /* eskisini sıfırlar */
void     relay_sm_off(relay_sm_t *r);
bool     relay_sm_eval(relay_sm_t *r, int64_t now_us);            /* true = röle çekili olmalı */
bool     relay_sm_active(const relay_sm_t *r);                    /* OFF dışında */
uint32_t relay_sm_remaining_s(const relay_sm_t *r, int64_t now_us);  /* sınırsızsa UINT32_MAX */
