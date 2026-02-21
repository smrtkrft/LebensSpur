#include "device_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "esp_timer.h"
#include "esp_log.h"

static device_log_entry_t s_entries[DEVICE_LOG_MAX_ENTRIES];
static int s_head = 0;
static int s_count = 0;

void device_log_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0;
    s_count = 0;
}

void device_log_add(device_log_type_t type, const char *fmt, ...)
{
    device_log_entry_t *e = &s_entries[s_head];
    e->timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
    e->type = type;

    va_list args;
    va_start(args, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, args);
    va_end(args);

    s_head = (s_head + 1) % DEVICE_LOG_MAX_ENTRIES;
    if (s_count < DEVICE_LOG_MAX_ENTRIES) s_count++;
}

int device_log_get_count(void)
{
    return s_count;
}

const device_log_entry_t *device_log_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    int pos = (s_head - 1 - index + DEVICE_LOG_MAX_ENTRIES) % DEVICE_LOG_MAX_ENTRIES;
    return &s_entries[pos];
}

static const char *log_type_str(device_log_type_t t)
{
    switch (t) {
        case LOG_TYPE_SYSTEM: return "system";
        case LOG_TYPE_WIFI:   return "wifi";
        case LOG_TYPE_TIMER:  return "timer";
        case LOG_TYPE_SMTP:   return "smtp";
        case LOG_TYPE_ERROR:  return "error";
        default:              return "unknown";
    }
}

int device_log_to_json(char *buf, int bufsz, int max_count)
{
    int count = s_count;
    if (max_count > 0 && max_count < count) count = max_count;

    int off = 0;
    off += snprintf(buf + off, bufsz - off, "{\"count\":%d,\"logs\":[", count);

    for (int i = 0; i < count && off < bufsz - 10; i++) {
        const device_log_entry_t *e = device_log_get(i);
        if (!e) break;
        if (i > 0) off += snprintf(buf + off, bufsz - off, ",");

        /* message icindeki tirnaklari escape et */
        char escaped[160];
        int ei = 0;
        for (int j = 0; e->message[j] && ei < (int)sizeof(escaped) - 2; j++) {
            if (e->message[j] == '"' || e->message[j] == '\\') {
                escaped[ei++] = '\\';
            }
            escaped[ei++] = e->message[j];
        }
        escaped[ei] = '\0';

        off += snprintf(buf + off, bufsz - off,
                        "{\"ts\":%lu,\"type\":\"%s\",\"msg\":\"%s\"}",
                        (unsigned long)e->timestamp,
                        log_type_str(e->type),
                        escaped);
    }

    off += snprintf(buf + off, bufsz - off, "]}");
    return off;
}
