#include "sk_event_bus.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sk_event_bus";

// Bumped 32 -> 64 to accommodate LS factory-reset subscribers (+5 from LS
// components) plus the existing sk_core subscribers. BF made the same bump
// after hitting the ceiling — see project_event_bus_capacity memory.
#define SK_EVT_MAX_SUBSCRIBERS   64
#define SK_EVT_FILTER_MAXLEN     48

typedef struct {
    bool                in_use;
    int                 id;
    char                filter[SK_EVT_FILTER_MAXLEN];
    sk_event_handler_t  handler;
    void               *user_ctx;
} subscriber_t;

static subscriber_t      s_subs[SK_EVT_MAX_SUBSCRIBERS];
static int               s_next_id = 1;
static uint32_t          s_seq     = 0;
static SemaphoreHandle_t s_mtx     = NULL;
static bool              s_ready   = false;

static bool filter_matches(const char *filter, const char *name)
{
    if (filter[0] == '*' && filter[1] == '\0') return true;
    size_t flen = strlen(filter);
    if (flen >= 2 && filter[flen - 2] == '.' && filter[flen - 1] == '*') {
        return strncmp(filter, name, flen - 1) == 0;  // include the dot
    }
    return strcmp(filter, name) == 0;
}

esp_err_t sk_event_bus_init(void)
{
    if (s_ready) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL) return ESP_ERR_NO_MEM;
    memset(s_subs, 0, sizeof(s_subs));
    s_seq = 0;
    s_next_id = 1;
    s_ready = true;
    return ESP_OK;
}

esp_err_t sk_event_bus_subscribe(const char *filter,
                                 sk_event_handler_t handler,
                                 void *user_ctx,
                                 int *out_sub_id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!filter || !handler) return ESP_ERR_INVALID_ARG;
    if (strlen(filter) >= SK_EVT_FILTER_MAXLEN) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < SK_EVT_MAX_SUBSCRIBERS; i++) {
        if (!s_subs[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(s_mtx);
        ESP_LOGW(TAG, "subscriber table full");
        return ESP_ERR_NO_MEM;
    }
    s_subs[idx].in_use = true;
    s_subs[idx].id = s_next_id++;
    strncpy(s_subs[idx].filter, filter, SK_EVT_FILTER_MAXLEN - 1);
    s_subs[idx].filter[SK_EVT_FILTER_MAXLEN - 1] = '\0';
    s_subs[idx].handler = handler;
    s_subs[idx].user_ctx = user_ctx;
    if (out_sub_id) *out_sub_id = s_subs[idx].id;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t sk_event_bus_unsubscribe(int sub_id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < SK_EVT_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].in_use && s_subs[i].id == sub_id) {
            s_subs[i].in_use = false;
            xSemaphoreGive(s_mtx);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mtx);
    return ESP_ERR_NOT_FOUND;
}

void sk_event_bus_publish(const char *name, const char *payload_json)
{
    if (!s_ready || !name) return;

    // Snapshot matching handlers under the lock, then call without holding it
    // so handlers are free to re-enter the bus (e.g. publish a follow-up).
    sk_event_handler_t matched_cb[SK_EVT_MAX_SUBSCRIBERS];
    void              *matched_ctx[SK_EVT_MAX_SUBSCRIBERS];
    int matched = 0;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t seq = ++s_seq;
    for (int i = 0; i < SK_EVT_MAX_SUBSCRIBERS; i++) {
        if (!s_subs[i].in_use) continue;
        if (!filter_matches(s_subs[i].filter, name)) continue;
        matched_cb[matched]  = s_subs[i].handler;
        matched_ctx[matched] = s_subs[i].user_ctx;
        matched++;
    }
    xSemaphoreGive(s_mtx);

    if (matched == 0) return;

    sk_event_t evt = {
        .name         = name,
        .seq          = seq,
        .ts_uptime_us = esp_timer_get_time(),
        .payload_json = payload_json,
    };

    for (int i = 0; i < matched; i++) {
        matched_cb[i](&evt, matched_ctx[i]);
    }
}

void sk_event_bus_publishf(const char *name, const char *payload_fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, payload_fmt);
    int n = vsnprintf(buf, sizeof(buf), payload_fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sk_event_bus_publish(name, buf);
}

uint32_t sk_event_bus_peek_seq(void)
{
    return s_seq;
}
