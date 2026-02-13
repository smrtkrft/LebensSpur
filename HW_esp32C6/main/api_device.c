#include "api_device.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "ota_manager.h"
#include "device_id.h"
#include "ext_flash.h"
#include "file_manager.h"
#include "time_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <string.h>

static const char *TAG = "API_DEVICE";

esp_err_t h_api_device_info(httpd_req_t *req)
{
    if (config_is_setup_completed() && !check_auth(req)) {
        return send_unauthorized(req);
    }
    ws_request_count++;

    cJSON *root = cJSON_CreateObject();
    if (!root) return web_server_send_error(req, 500, "No memory");

    const char *dev_id = device_id_get();

    cJSON_AddStringToObject(root, "device_id", dev_id);
    cJSON_AddStringToObject(root, "firmware", ota_manager_get_current_version());
    cJSON_AddStringToObject(root, "hostname", dev_id);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON_AddStringToObject(root, "chip_model", "ESP32-C6");
    cJSON_AddNumberToObject(root, "chip_cores", chip.cores);
    cJSON_AddNumberToObject(root, "cpu_freq_mhz", 160);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);

    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t heap_free = esp_get_free_heap_size();
    size_t heap_min = esp_get_minimum_free_heap_size();
    cJSON_AddNumberToObject(root, "heap_total", (double)heap_total);
    cJSON_AddNumberToObject(root, "heap_free", (double)heap_free);
    cJSON_AddNumberToObject(root, "heap_min_free", (double)heap_min);

    const esp_partition_t *running = esp_ota_get_running_partition();
    uint32_t int_flash = 0;
    uint32_t app_size = 0;
    if (running) {
        int_flash = 4 * 1024 * 1024;
        app_size = running->size;
    }
    cJSON_AddNumberToObject(root, "int_flash_total", (double)int_flash);
    cJSON_AddNumberToObject(root, "app_size", (double)app_size);
    cJSON_AddNumberToObject(root, "ota_size", (double)app_size);
    cJSON_AddNumberToObject(root, "nvs_size", 24576.0);

    uint32_t ext_total = ext_flash_get_size();
    cJSON_AddNumberToObject(root, "ext_flash_total", (double)ext_total);

    uint32_t fs_total = 0, fs_used = 0;
    file_manager_get_info(&fs_total, &fs_used);
    cJSON_AddNumberToObject(root, "fs_cfg_total", (double)fs_total);
    cJSON_AddNumberToObject(root, "fs_cfg_used", (double)fs_used);
    cJSON_AddNumberToObject(root, "fs_gui_total", 0);
    cJSON_AddNumberToObject(root, "fs_gui_used", 0);
    cJSON_AddNumberToObject(root, "fs_data_total", 0);
    cJSON_AddNumberToObject(root, "fs_data_used", 0);

    bool sta_connected = wifi_manager_is_connected();
    cJSON_AddBoolToObject(root, "sta_connected", sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", wifi_manager_get_ip());

    wifi_ap_record_t ap_info;
    if (sta_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddStringToObject(root, "sta_ssid", (const char *)ap_info.ssid);
        cJSON_AddNumberToObject(root, "sta_rssi", ap_info.rssi);
    } else {
        cJSON_AddStringToObject(root, "sta_ssid", "");
        cJSON_AddNumberToObject(root, "sta_rssi", 0);
    }

    ls_wifi_config_t wcfg;
    bool ap_active = true;
    if (config_load_wifi(&wcfg) == ESP_OK) {
        ap_active = wcfg.ap_mode_enabled;
    }
    cJSON_AddBoolToObject(root, "ap_active", ap_active);
    cJSON_AddStringToObject(root, "ap_ip", wifi_manager_get_ap_ip());
    cJSON_AddStringToObject(root, "ap_ssid", wifi_manager_get_ap_ssid());

    cJSON_AddNumberToObject(root, "uptime_s", (double)time_manager_get_uptime_sec());
    cJSON_AddNumberToObject(root, "reset_reason", (double)esp_reset_reason());

    cJSON_AddBoolToObject(root, "ntp_synced", time_manager_is_synced());
    char time_str[TIME_STR_MAX_LEN];
    time_manager_get_time_str(time_str, sizeof(time_str), NULL);
    cJSON_AddStringToObject(root, "time", time_str);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

esp_err_t h_api_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    uint32_t total = 0, used = 0;
    file_manager_get_info(&total, &used);

    char json[256];
    snprintf(json, sizeof(json),
        "{\"uptime_ms\":%llu,\"heap_free\":%lu,\"heap_min\":%lu,"
        "\"flash_total\":%lu,\"flash_used\":%lu,\"requests\":%lu}",
        (unsigned long long)(esp_timer_get_time() / 1000),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        total, used, ws_request_count);

    return web_server_send_json(req, json);
}

esp_err_t h_api_reboot(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;
    ESP_LOGW(TAG, "Reboot istegi alindi");

    web_server_send_json(req, "{\"success\":true,\"message\":\"Rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t h_api_factory_reset(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;
    ESP_LOGW(TAG, "Factory reset istegi alindi");

    config_factory_reset();
    web_server_send_json(req, "{\"success\":true,\"message\":\"Factory reset done, rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
