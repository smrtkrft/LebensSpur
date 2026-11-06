/*
 * core — app_core görevi (öncelik 8, 6 KB, TWDT abonesi): tek olay kuyruğu, tüm uygulama kararları.
 * Callback'ler (esp_timer, GPIO ISR, WiFi, HTTP) yalnız core_post ile olay bırakır.
 * Flash dışında bloklamaz: tek I/O, durum değişince NVS "rt" blob'u (süresi ölçülür).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "outbox.h"
#include "relay_sm.h"
#include "timer_sm.h"

typedef enum { CHK_BUTTON, CHK_WEB, CHK_API_KEY } checkin_source_t;

typedef enum {
    EV_TICK, EV_BUTTON_PRESS, EV_BUTTON_WINDOW, EV_CHECKIN, EV_TIMER_CANCEL, EV_VACATION_SET,
    EV_VACATION_CANCEL, EV_RELAY_TEST, EV_DELIVERY_RESULT, EV_NET_LINK, EV_CFG_CHANGED, EV_FACTORY_RESET,
    EV_TEST_SEND,
} core_event_type_t;

typedef struct {
    core_event_type_t type;
    uint32_t ticket;                            /* core_post_sync: işlendi bildirimi (0 = yok) */
    union {
        struct { checkin_source_t src; } checkin;
        struct { uint8_t days; } vacation;
        struct { uint16_t duration_s; } relay_test;
        struct { uint16_t seq; uint8_t result; char err[48]; } delivery;   /* result: channel_result_t */
        struct { bool up; } link;
        struct { uint8_t section; } cfg;                                   /* CFG_SEC__COUNT = grup */
        struct { uint8_t level; } factory;
        struct { uint8_t ch; uint16_t job_id; } test;                      /* channel_t, jobs kimliği */
    } u;
} core_event_t;

typedef struct {
    timer_state_t state;
    uint32_t total_s, remaining_s, vac_total_s, vac_remaining_s;
    uint8_t  warn_n, warn_fired;
    uint32_t warn_off_s[WARN_MAX];
    bool     relay_on;
    uint32_t relay_remaining_s;                 /* UINT32_MAX = sınırsız */
    ob_summary_t trigger;
    bool     link_up;
    checkin_source_t last_checkin_src;
    uint32_t last_checkin_up_s;                 /* 0 = yok */
    uint32_t q_drop;
    uint16_t version;                           /* her değişimde artar (SSE) */
} app_status_t;

/* delivery görevine iş verme kancası (adım 4). false → meşgul/yok; iş sonra yeniden denenir. */
typedef bool (*core_dispatch_t)(const ob_job_t *job, uint16_t trigger_seq, uint32_t remaining_s, uint8_t warn_n);
typedef void (*core_status_cb_t)(void);         /* durum değişti (SSE push); kısa olmalı */

esp_err_t core_start(void);
esp_err_t core_post(const core_event_t *ev);                 /* 0 bekleme; dolu → ESP_ERR_NO_MEM */
/* Gönder ve çekirdek işleyip durumu yayınlayana kadar bekle (API: yanıt döndüğünde etki görünür).
 * ESP_ERR_TIMEOUT: olay kuyrukta, sonra işlenecek. Yalnız görev bağlamından. */
esp_err_t core_post_sync(core_event_t *ev, uint32_t timeout_ms);
void      core_post_from_isr(const core_event_t *ev);
void      core_get_status(app_status_t *out);
void      core_set_dispatch(core_dispatch_t fn);
void      core_set_status_cb(core_status_cb_t fn);
void      core_quiet_for(uint32_t seconds);                 /* teslimatı geciktir (brownout açılışı, K5) */
