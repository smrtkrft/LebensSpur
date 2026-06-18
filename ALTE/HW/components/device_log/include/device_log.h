#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LOG_TYPE_SYSTEM,
    LOG_TYPE_WIFI,
    LOG_TYPE_TIMER,
    LOG_TYPE_SMTP,
    LOG_TYPE_ERROR,
    LOG_TYPE_OTA,
    LOG_TYPE_RESTART,
    LOG_TYPE_HW
} device_log_type_t;

typedef struct {
    uint32_t timestamp;      // uptime saniye (boot'tan itibaren)
    uint32_t boot_count;     // hangi boot'ta olusturuldu
    device_log_type_t type;
    char message[120];
} device_log_entry_t;

#define DEVICE_LOG_MAX_ENTRIES 100

// Faz 1: RAM buffer baslatma (ext_flash'tan ONCE cagrilir)
void device_log_init(void);

// Faz 2: Ext flash'tan kalici loglari yukle (ext_flash_init'ten SONRA cagrilir)
void device_log_persist_init(void);

// Log ekle (RAM + flash'a yazar)
void device_log_add(device_log_type_t type, const char *fmt, ...);

int device_log_get_count(void);
const device_log_entry_t *device_log_get(int index);
int device_log_to_json(char *buf, int bufsz, int max_count);

// Boot sayacini dondur
uint32_t device_log_get_boot_count(void);
