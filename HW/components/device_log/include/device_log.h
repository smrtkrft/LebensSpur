#pragma once

#include <stdint.h>

typedef enum {
    LOG_TYPE_SYSTEM,
    LOG_TYPE_WIFI,
    LOG_TYPE_TIMER,
    LOG_TYPE_SMTP,
    LOG_TYPE_ERROR
} device_log_type_t;

typedef struct {
    uint32_t timestamp;
    device_log_type_t type;
    char message[80];
} device_log_entry_t;

#define DEVICE_LOG_MAX_ENTRIES 50

void device_log_init(void);
void device_log_add(device_log_type_t type, const char *fmt, ...);
int device_log_get_count(void);
const device_log_entry_t *device_log_get(int index);
int device_log_to_json(char *buf, int bufsz, int max_count);
