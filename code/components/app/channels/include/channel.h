/*
 * channel — dört kanalın tek arayüzü (delivery görevinde çağrılır; bloklar, son tarihlidir).
 * Sonuç: CR_OK, CR_RETRY (geçici), CR_PERM (kalıcı), CR_CONFIG (ayar hatası; deneme harcamaz).
 */
#pragma once
#include <stdint.h>
#include "outbox.h"

typedef struct {
    const ob_job_t *job;
    uint16_t trigger_seq;          /* Message-ID / Idempotency-Key */
    uint32_t remaining_s;          /* erken uyarıda kalan süre ({remaining}) */
    uint8_t  warn_n;               /* {n} */
} channel_ctx_t;

channel_result_t channel_send(const channel_ctx_t *c, char *err, size_t errlen);

/* kanal uygulamaları (ch_*.c) */
channel_result_t ch_smtp_send(const channel_ctx_t *c, char *err, size_t errlen);
channel_result_t ch_telegram_send(const channel_ctx_t *c, char *err, size_t errlen);
channel_result_t ch_webhook_send(const channel_ctx_t *c, char *err, size_t errlen);
channel_result_t ch_mqtt_send(const channel_ctx_t *c, char *err, size_t errlen);

/* ortak: şablon değişkenleri (zaman, cihaz, kalan süre metni) */
#include "tmpl.h"
void channel_vars(const channel_ctx_t *c, tmpl_vars_t *v, char *time_buf, size_t tlen, char *rem_buf, size_t rlen);
