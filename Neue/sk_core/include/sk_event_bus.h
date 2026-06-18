#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// An event as delivered to subscribers. All fields are owned by the bus and
// valid only for the duration of the handler call — copy strings out if you
// need to keep them.
typedef struct {
    const char *name;         // e.g. "timer.tick", not NULL
    uint32_t    seq;          // monotonic, starts at 1
    int64_t     ts_uptime_us; // esp_timer_get_time() at publish
    const char *payload_json; // JSON object body, may be NULL if event has no data
} sk_event_t;

typedef void (*sk_event_handler_t)(const sk_event_t *evt, void *user_ctx);

// Initialize the bus. Safe to call multiple times.
esp_err_t sk_event_bus_init(void);

// Subscribe to events whose `name` matches `filter`. Filter grammar:
//   "foo.bar"   — exact match
//   "foo.*"     — any name starting with "foo."
//   "*"         — all events
// Returns a handle in `out_sub_id` for later unsubscribe. `user_ctx` is
// passed verbatim to the handler.
esp_err_t sk_event_bus_subscribe(const char         *filter,
                                 sk_event_handler_t  handler,
                                 void               *user_ctx,
                                 int                *out_sub_id);

esp_err_t sk_event_bus_unsubscribe(int sub_id);

// Publish an event. `payload_json` may be NULL or a JSON object/array string
// (caller-owned; the bus copies it for each delivery if needed). Handlers
// are called synchronously on the publisher task — keep them short. Use an
// internal FreeRTOS queue inside a handler if heavy work is required.
void sk_event_bus_publish(const char *name, const char *payload_json);

// Convenience: publish with a printf-formatted JSON payload.
void sk_event_bus_publishf(const char *name, const char *payload_fmt, ...);

// Returns the next sequence number without publishing — rarely needed, useful
// when building a snapshot that must align with a future event stream.
uint32_t sk_event_bus_peek_seq(void);

#ifdef __cplusplus
}
#endif
