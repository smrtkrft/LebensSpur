/**
 * @file device_id.c
 * @brief Device ID implementation for LebensSpur ESP32-C6
 * 
 * Generates unique device ID from ESP32-C6 chip's MAC address.
 * Format: LS-XXXXXXXXXXXX (LS- prefix + 12 hex chars from MAC)
 */

#include "device_id.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "device_id";

/** Device ID string storage */
static char s_device_id[DEVICE_ID_MAX_LEN] = {0};

/** Hex string without prefix */
static char s_device_hex[13] = {0};

/** Chip ID (MAC address) */
static uint8_t s_chip_id[6] = {0};

/** Initialization flag */
static bool s_initialized = false;

/**
 * @brief Convert byte to uppercase hex characters
 */
static void byte_to_hex(uint8_t byte, char *hex)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    hex[0] = hex_chars[(byte >> 4) & 0x0F];
    hex[1] = hex_chars[byte & 0x0F];
}

int device_id_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Get base MAC address (unique per chip)
    esp_err_t ret = esp_efuse_mac_get_default(s_chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    // Convert to hex string (uppercase)
    for (int i = 0; i < 6; i++) {
        byte_to_hex(s_chip_id[i], &s_device_hex[i * 2]);
    }
    s_device_hex[12] = '\0';

    // Build device ID: LS-XXXXXXXXXXXX
    snprintf(s_device_id, DEVICE_ID_MAX_LEN, "%s%s", DEVICE_ID_PREFIX, s_device_hex);

    s_initialized = true;

    ESP_LOGI(TAG, "Device ID initialized: %s", s_device_id);
    ESP_LOGI(TAG, "Chip ID (MAC): %02X:%02X:%02X:%02X:%02X:%02X",
             s_chip_id[0], s_chip_id[1], s_chip_id[2],
             s_chip_id[3], s_chip_id[4], s_chip_id[5]);

    return ESP_OK;
}

const char* device_id_get(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Not initialized, returning empty string");
        return "";
    }
    return s_device_id;
}

int device_id_get_chip_id(uint8_t chip_id[6])
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(chip_id, s_chip_id, 6);
    return ESP_OK;
}

const char* device_id_get_hex(void)
{
    if (!s_initialized) {
        return "";
    }
    return s_device_hex;
}

bool device_id_is_valid(void)
{
    return s_initialized;
}
