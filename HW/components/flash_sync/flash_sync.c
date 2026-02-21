/**
 * LebensSpur - Flash Senkronizasyon Modulu
 * Dahili NVS (config) ile harici flash arasinda senkronizasyon
 */

#include "flash_sync.h"
#include "ext_flash.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "FLASH_SYNC";
static nvs_handle_t s_nvs_handle;
static bool s_initialized = false;

#define NVS_NAMESPACE "ls_config"

esp_err_t flash_sync_init(void)
{
    ESP_LOGI(TAG, "Flash senkronizasyon baslatiliyor...");

    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS acilamadi: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Flash senkronizasyon hazir");
    return ESP_OK;
}

esp_err_t flash_sync_save_config(const char *key, const char *value)
{
    if (!s_initialized || !key || !value) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = nvs_set_str(s_nvs_handle, key, value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS yazma hatasi [%s]: %s", key, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit hatasi: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t flash_sync_load_config(const char *key, char *value, size_t *buf_size)
{
    if (!s_initialized || !key || !value || !buf_size) {
        return ESP_ERR_INVALID_STATE;
    }

    return nvs_get_str(s_nvs_handle, key, value, buf_size);
}

esp_err_t flash_sync_to_external(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!ext_flash_is_user_data_mounted()) {
        ESP_LOGW(TAG, "User Data partition mount degil, senkronizasyon atlanacak");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Dahili config -> Harici flash senkronizasyon basliyor...");

    // Config.json olustur ve harici flash'a yaz
    char config_json[512];
    char wifi_ssid[33] = "";
    size_t ssid_len = sizeof(wifi_ssid);

    // WiFi bilgilerini oku
    nvs_handle_t wifi_nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &wifi_nvs) == ESP_OK) {
        nvs_get_str(wifi_nvs, "ssid", wifi_ssid, &ssid_len);
        nvs_close(wifi_nvs);
    }

    snprintf(config_json, sizeof(config_json),
             "{\"wifi_ssid\":\"%s\",\"version\":\"1.0\",\"device\":\"LebensSpur\"}",
             wifi_ssid);

    esp_err_t ret = ext_flash_write_file("/config.json", config_json, strlen(config_json));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config yazma hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Senkronizasyon tamamlandi");
    return ESP_OK;
}
