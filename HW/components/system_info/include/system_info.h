#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    char chip_model[16];
    uint8_t cores;
    uint32_t cpu_freq_mhz;
    uint32_t flash_size_kb;
    uint32_t flash_used_kb;
    uint32_t heap_total;
    uint32_t heap_free;
    uint32_t heap_min_free;
    int64_t uptime_seconds;
    int wifi_rssi;
    char reset_reason[32];
    char fw_version[16];
} system_info_t;

esp_err_t system_info_get(system_info_t *info);
