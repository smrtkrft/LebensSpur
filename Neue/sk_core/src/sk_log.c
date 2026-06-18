// sk_log.c — structured event log ring buffer (NimBLE-safe via async queue).
//
// Design — why a queue + drain task instead of writing the ring directly?
// The earlier vprintf hook tried to acquire the ring mutex from whichever
// task happened to emit an ESP_LOG line, including the NimBLE host. NimBLE
// is latency-sensitive and any blocked mutex acquisition there can stall
// the BLE handshake (this was the exact symptom of the previous abandoned
// hook). Here every caller only does:
//   1. format the message into a stack buffer
//   2. xQueueSendToBack(0)  (non-blocking, drops on full queue)
// The drain task is the sole writer to the ring. Mutex contention is
// confined to one pair of tasks (the drain + logs.get reader).

#include "sk_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// === Tunables ===============================================================

#define SK_LOG_RING_CAPACITY    64   // entries kept in RAM (oldest dropped)
#define SK_LOG_MSG_MAX          128  // bytes per msg payload (excluding NUL)
#define SK_LOG_QUEUE_DEPTH      32   // backpressure buffer before drop
#define SK_LOG_TASK_STACK       3072 // drain task stack
#define SK_LOG_TASK_PRIO        3    // low priority; logging is background

// === Storage layout =========================================================

typedef struct {
    int64_t        ts_unix;
    int64_t        ts_uptime_us;
    sk_log_level_t level;
    const char    *tag;             // static lifetime guaranteed by caller
    const char    *event;           // static lifetime guaranteed by caller
    char           msg[SK_LOG_MSG_MAX + 1];
} sk_log_record_t;

static sk_log_record_t   s_ring[SK_LOG_RING_CAPACITY];
static int               s_ring_head  = 0;   // next slot to write
static int               s_ring_count = 0;   // 0..CAPACITY
static SemaphoreHandle_t s_ring_mtx   = NULL;

static QueueHandle_t     s_queue      = NULL;
static TaskHandle_t      s_task       = NULL;
static bool              s_ready      = false;

// === Helpers ================================================================

const char *sk_log_level_str(sk_log_level_t level)
{
    switch (level) {
        case SK_LOG_DEBUG: return "D";
        case SK_LOG_INFO:  return "I";
        case SK_LOG_WARN:  return "W";
        case SK_LOG_ERROR: return "E";
    }
    return "?";
}

bool sk_log_level_parse(const char *s, sk_log_level_t *out)
{
    if (!s || !out) return false;
    if (!strcasecmp(s, "debug") || !strcasecmp(s, "d")) { *out = SK_LOG_DEBUG; return true; }
    if (!strcasecmp(s, "info")  || !strcasecmp(s, "i")) { *out = SK_LOG_INFO;  return true; }
    if (!strcasecmp(s, "warn")  || !strcasecmp(s, "w")) { *out = SK_LOG_WARN;  return true; }
    if (!strcasecmp(s, "error") || !strcasecmp(s, "e")) { *out = SK_LOG_ERROR; return true; }
    return false;
}

static int64_t current_unix_or_zero(void)
{
    time_t now = time(NULL);
    // Treat any post-2023 epoch as "real time set". Earlier than that and
    // we report 0 so the SKAPP UI can mark the entry as time-unknown.
    return (now > 1700000000) ? (int64_t)now : 0;
}

// === Drain task =============================================================

static void ring_write_locked(const sk_log_record_t *rec)
{
    s_ring[s_ring_head] = *rec;
    s_ring_head = (s_ring_head + 1) % SK_LOG_RING_CAPACITY;
    if (s_ring_count < SK_LOG_RING_CAPACITY) s_ring_count++;
}

static void log_task(void *arg)
{
    (void)arg;
    sk_log_record_t rec;
    for (;;) {
        if (xQueueReceive(s_queue, &rec, portMAX_DELAY) != pdTRUE) continue;
        if (!s_ring_mtx) continue;
        if (xSemaphoreTake(s_ring_mtx, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        ring_write_locked(&rec);
        xSemaphoreGive(s_ring_mtx);
    }
}

// === Public API =============================================================

esp_err_t sk_log_init(void)
{
    if (s_ready) return ESP_OK;

    s_ring_mtx = xSemaphoreCreateMutex();
    if (!s_ring_mtx) return ESP_ERR_NO_MEM;

    s_queue = xQueueCreate(SK_LOG_QUEUE_DEPTH, sizeof(sk_log_record_t));
    if (!s_queue) {
        vSemaphoreDelete(s_ring_mtx);
        s_ring_mtx = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(log_task, "sk_log", SK_LOG_TASK_STACK, NULL,
                                SK_LOG_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        vSemaphoreDelete(s_ring_mtx);
        s_ring_mtx = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    return ESP_OK;
}

void sk_log_eventv(sk_log_level_t level,
                   const char *tag, const char *event,
                   const char *fmt, va_list ap)
{
    if (!s_ready || !s_queue) return;
    if (!tag) tag = "?";
    if (!event) event = "?";

    sk_log_record_t rec = {
        .ts_unix      = current_unix_or_zero(),
        .ts_uptime_us = esp_timer_get_time(),
        .level        = level,
        .tag          = tag,
        .event        = event,
    };

    if (fmt && fmt[0]) {
        int n = vsnprintf(rec.msg, sizeof(rec.msg), fmt, ap);
        if (n < 0) rec.msg[0] = '\0';
        // vsnprintf already NUL-terminates within buffer bounds
        (void)n;
    } else {
        rec.msg[0] = '\0';
    }

    // Non-blocking: if the queue is full we drop the entry rather than
    // stall the caller (critical for NimBLE host task).
    (void)xQueueSendToBack(s_queue, &rec, 0);
}

void sk_log_event(sk_log_level_t level,
                  const char *tag, const char *event,
                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sk_log_eventv(level, tag, event, fmt, ap);
    va_end(ap);
}

void sk_log_walk(sk_log_level_t min_level, size_t max_count,
                 sk_log_walk_cb_t cb, void *user)
{
    if (!s_ready || !cb || !s_ring_mtx) return;
    if (xSemaphoreTake(s_ring_mtx, portMAX_DELAY) != pdTRUE) return;

    int total = s_ring_count;
    int start = (s_ring_head - total + SK_LOG_RING_CAPACITY) % SK_LOG_RING_CAPACITY;
    size_t emitted = 0;
    for (int i = 0; i < total; i++) {
        if (emitted >= max_count) break;
        int idx = (start + i) % SK_LOG_RING_CAPACITY;
        const sk_log_record_t *r = &s_ring[idx];
        if (r->level < min_level) continue;

        sk_log_entry_view_t v = {
            .ts_unix      = r->ts_unix,
            .ts_uptime_us = r->ts_uptime_us,
            .level        = r->level,
            .tag          = r->tag,
            .event        = r->event,
            .msg          = r->msg,
        };
        bool keep = cb(&v, user);
        emitted++;
        if (!keep) break;
    }

    xSemaphoreGive(s_ring_mtx);
}
