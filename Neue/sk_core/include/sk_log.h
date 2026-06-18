#pragma once

// sk_log — structured event log baseline for all SmartKraft devices.
//
// See esp32/COMMON_LOG_SPEC.md for the level/tag/event vocabulary every
// firmware must emit. This header is the API every component calls to
// write into the on-device ring buffer that `logs.get` returns.
//
// Concurrency: sk_log_event is non-blocking and safe to call from any
// task including NimBLE host. The call formats into a stack buffer and
// pushes a record onto a FreeRTOS queue; a dedicated background task
// drains the queue and writes the ring under a mutex. Queue full means
// the entry is silently dropped (best-effort), never blocks the caller.

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SK_LOG_DEBUG = 0,
    SK_LOG_INFO  = 1,
    SK_LOG_WARN  = 2,
    SK_LOG_ERROR = 3,
} sk_log_level_t;

// Start the queue + background drain task. Idempotent. Called from
// sk_baseline_init early in boot. Returns ESP_OK or ESP_ERR_NO_MEM if
// the queue/task could not be created.
esp_err_t sk_log_init(void);

// Write one event. `tag` is the component name (e.g. "wifi"), `event`
// is the dot-event name (e.g. "connect.fail"). `fmt` and args build the
// `msg` payload (key=value style recommended, e.g. "ssid=Home rssi=-52").
// All string arguments to the resulting `msg` must outlive the call;
// the implementation copies into the queue record. `tag` and `event`
// pointers are stored as-is so they MUST have static lifetime (string
// literals).
void sk_log_event(sk_log_level_t level,
                  const char *tag, const char *event,
                  const char *fmt, ...) __attribute__((format(printf, 4, 5)));

// va_list variant for wrappers.
void sk_log_eventv(sk_log_level_t level,
                   const char *tag, const char *event,
                   const char *fmt, va_list ap);

// Convenience macros — preferred call site form.
#define SK_LOG_D(tag, event, ...) sk_log_event(SK_LOG_DEBUG, (tag), (event), __VA_ARGS__)
#define SK_LOG_I(tag, event, ...) sk_log_event(SK_LOG_INFO,  (tag), (event), __VA_ARGS__)
#define SK_LOG_W(tag, event, ...) sk_log_event(SK_LOG_WARN,  (tag), (event), __VA_ARGS__)
#define SK_LOG_E(tag, event, ...) sk_log_event(SK_LOG_ERROR, (tag), (event), __VA_ARGS__)

// Read-only view of one ring entry, passed to walk callbacks.
typedef struct {
    int64_t        ts_unix;       // 0 if device time not yet set
    int64_t        ts_uptime_us;  // monotonic, always valid
    sk_log_level_t level;
    const char    *tag;           // static literal, no lifetime concern
    const char    *event;         // static literal
    const char    *msg;           // valid only during the callback
} sk_log_entry_view_t;

// Return false from the callback to stop walking early.
typedef bool (*sk_log_walk_cb_t)(const sk_log_entry_view_t *entry, void *user);

// Walk recent entries (oldest first) up to max_count, filtering by
// minimum severity. Holds the ring mutex during traversal so the
// callback must not call back into sk_log_event (would queue but the
// callback should still stay short).
void sk_log_walk(sk_log_level_t min_level, size_t max_count,
                 sk_log_walk_cb_t cb, void *user);

// Returns the textual level name ("D" / "I" / "W" / "E"). Always a
// valid one-char static string.
const char *sk_log_level_str(sk_log_level_t level);

// Parse a level token ("debug","info","warn","error" or "d"/"i"/"w"/"e",
// case-insensitive). Returns true on success.
bool sk_log_level_parse(const char *s, sk_log_level_t *out);

#ifdef __cplusplus
}
#endif
