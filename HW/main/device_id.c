/**
 * Device ID - ESP32-C6 Benzersiz Cihaz Kimliği
 *
 * MAC adresinin 48 bitini base36'ya çevirir.
 * 48 bit → max 10 base36 karakter (36^10 = ~3.6×10^15 > 2^48 = ~2.8×10^14)
 */

#include "device_id.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "DEVICE_ID";

static uint8_t s_mac[6] = {0};
static char s_id[DEVICE_ID_LENGTH] = {0};
static bool s_initialized = false;

static const char BASE36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

/**
 * 6 byte MAC → 10 karakter base36
 */
static void mac_to_base36(const uint8_t *mac, char *out)
{
    uint64_t num = 0;
    for (int i = 0; i < 6; i++) {
        num = (num << 8) | mac[i];
    }

    char tmp[11];
    int idx = 0;

    while (num > 0 && idx < 10) {
        tmp[idx++] = BASE36[num % 36];
        num /= 36;
    }

    // Kalan basamakları '0' ile doldur
    while (idx < 10) {
        tmp[idx++] = '0';
    }

    // Ters çevir (MSB önce)
    for (int i = 0; i < 10; i++) {
        out[i] = tmp[9 - i];
    }
    out[10] = '\0';
}

esp_err_t device_id_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // WiFi STA MAC adresini oku
    esp_err_t ret = esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi MAC okunamadı, efuse deneniyor...");
        ret = esp_efuse_mac_get_default(s_mac);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MAC adresi alınamadı: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // ID oluştur: "LS-" + base36(MAC)
    strcpy(s_id, DEVICE_ID_PREFIX);
    mac_to_base36(s_mac, s_id + strlen(DEVICE_ID_PREFIX));

    s_initialized = true;
    ESP_LOGI(TAG, "ID: %s", s_id);

    return ESP_OK;
}

const char *device_id_get(void)
{
    return s_initialized ? s_id : "LS-UNKNOWN";
}

esp_err_t device_id_get_str(char *buffer, size_t len)
{
    if (!buffer || len < DEVICE_ID_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(buffer, device_id_get(), len - 1);
    buffer[len - 1] = '\0';
    return ESP_OK;
}

esp_err_t device_id_get_mac(uint8_t *mac)
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(mac, s_mac, 6);
    return ESP_OK;
}

void device_id_print_info(void)
{
    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ Device ID:  %s", s_id);
    ESP_LOGI(TAG, "│ MAC:        %02X:%02X:%02X:%02X:%02X:%02X",
             s_mac[0], s_mac[1], s_mac[2],
             s_mac[3], s_mac[4], s_mac[5]);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
