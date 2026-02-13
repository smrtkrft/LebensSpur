/**
 * LebensSpur - ESP32-C6 Ana Program
 *
 * Dead Man's Switch IoT cihazi.
 * Dahili Flash (4MB): Firmware, OTA, NVS
 * Harici Flash (32MB): LittleFS - web, log, config, data
 *
 * Baslangic sirasi (katman bagimliliklarina gore):
 *   NVS -> Katman 0 -> Katman 1 -> Katman 2 -> Katman 3 -> Katman 4
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

// Katman 0 - Donanim
#include "device_id.h"
#include "ext_flash.h"
#include "relay_manager.h"
#include "button_manager.h"

// Katman 1 - Altyapi
#include "file_manager.h"
#include "time_manager.h"
#include "log_manager.h"

// Katman 2 - Yapilandirma
#include "config_manager.h"
#include "session_auth.h"

// Katman 3 - Iletisim
#include "wifi_manager.h"
#include "mail_sender.h"
#include "gui_downloader.h"
#include "ota_manager.h"

// Katman 4 - Uygulama
#include "timer_scheduler.h"
#include "web_assets.h"
#include "web_server.h"

static const char *TAG = "MAIN";

// ============================================================================
// WiFi baglanti callback - NTP sync baslat
// ============================================================================
static void wifi_event_handler(bool connected)
{
    if (connected) {
        ESP_LOGI(TAG, "WiFi baglandi - NTP senkronizasyonu baslatiliyor");
        time_manager_sync();
    } else {
        ESP_LOGW(TAG, "WiFi baglantisi kesildi");
    }
}

// ============================================================================
// Button callback - kisa/uzun/cok uzun basma
// ============================================================================
static void button_event_handler(button_event_t event)
{
    switch (event) {
        case BUTTON_EVENT_PRESS:
            ESP_LOGI(TAG, "Buton: Kisa basma - Timer sifirlama");
            timer_reset();
            break;

        case BUTTON_EVENT_LONG_PRESS:
            ESP_LOGI(TAG, "Buton: Uzun basma - Relay tetikleme");
            relay_trigger();
            break;

        case BUTTON_EVENT_VERY_LONG:
            ESP_LOGW(TAG, "Buton: Cok uzun basma - Factory reset");
            config_factory_reset();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            break;

        default:
            break;
    }
}

// ============================================================================
// Config'den relay ayarlarini yukle ve uygula
// ============================================================================
static void apply_relay_config(void)
{
    ls_relay_config_t cfg;
    if (config_load_relay(&cfg) == ESP_OK) {
        relay_config_t rc = {
            .inverted          = cfg.inverted,
            .delay_seconds     = cfg.delay_seconds,
            .duration_seconds  = cfg.duration_seconds,
            .pulse_enabled     = cfg.pulse_enabled,
            .pulse_on_ms       = cfg.pulse_on_ms,
            .pulse_off_ms      = cfg.pulse_off_ms,
        };
        relay_set_config(&rc);
        ESP_LOGI(TAG, "Relay config yuklendi");
    }
}

// ============================================================================
// app_main
// ============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "       LebensSpur ESP32-C6");
    ESP_LOGI(TAG, "============================================");

    esp_err_t ret;

    // ========================================
    // NVS (ESP-IDF temel gereksinim)
    // ========================================
    ESP_LOGI(TAG, "[ 1/14] NVS");
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ========================================
    // Katman 0 - Donanim
    // ========================================
    ESP_LOGI(TAG, "[ 2/14] Device ID");
    ret = device_id_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "        %s", device_id_get());
    } else {
        ESP_LOGE(TAG, "        Device ID basarisiz!");
    }

    ESP_LOGI(TAG, "[ 3/14] Harici Flash (W25Q256)");
    ret = ext_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "        Harici flash basarisiz!");
    }

    ESP_LOGI(TAG, "[ 4/14] Relay Manager");
    relay_manager_init();

    ESP_LOGI(TAG, "[ 5/14] Button Manager");
    button_manager_init();
    button_set_callback(button_event_handler);

    // ========================================
    // Katman 1 - Altyapi
    // ========================================
    ESP_LOGI(TAG, "[ 6/14] Dosya Sistemi (LittleFS)");
    ret = file_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "        LittleFS basarisiz!");
    }

    ESP_LOGI(TAG, "[ 7/14] Time Manager");
    time_manager_init();

    ESP_LOGI(TAG, "[ 8/14] Log Manager");
    log_manager_init();

    // ========================================
    // Katman 2 - Yapilandirma
    // ========================================
    ESP_LOGI(TAG, "[ 9/14] Config Manager");
    config_manager_init();

    // Relay config'i config'den yukle ve uygula
    apply_relay_config();

    ESP_LOGI(TAG, "[10/14] Session Auth");
    session_auth_init();

    // ========================================
    // Katman 3 - Iletisim
    // ========================================
    ESP_LOGI(TAG, "[11/14] WiFi Manager");
    wifi_manager_set_callback(wifi_event_handler);
    wifi_manager_init();

    ESP_LOGI(TAG, "[12/14] Mail Sender / OTA / GUI Downloader");
    mail_sender_init();
    ota_manager_init();
    gui_downloader_init();

    // ========================================
    // Katman 4 - Uygulama
    // ========================================
    ESP_LOGI(TAG, "[13/14] Timer Scheduler");
    timer_scheduler_init();

    ESP_LOGI(TAG, "[14/14] Web Server");
    web_server_start();

    // ========================================
    // Sistem hazir
    // ========================================
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "       Sistem Hazir!");
    ESP_LOGI(TAG, "============================================");

    if (!config_is_setup_completed()) {
        ESP_LOGW(TAG, "Ilk kurulum gerekiyor: http://192.168.4.1/setup.html");
    }

    // Tum modullerin bilgilerini yazdir
    ESP_LOGI(TAG, "");
    device_id_print_info();
    ext_flash_print_info();
    file_manager_print_info();
    time_manager_print_info();
    log_manager_print_info();
    ota_manager_print_info();
    wifi_manager_print_info();
    web_server_print_stats();
    timer_print_stats();
    mail_print_stats();
    relay_print_stats();
    button_print_stats();

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device:  %s", device_id_get());
    ESP_LOGI(TAG, "AP SSID: %s", wifi_manager_get_ap_ssid());
    ESP_LOGI(TAG, "Web:     http://192.168.4.1/");
    ESP_LOGI(TAG, "");

    // ========================================
    // Ana dongu - buton ve relay tick (10ms)
    // ========================================
    uint32_t loop_count = 0;

    while (1) {
        button_tick();
        relay_tick();

        // Her 10 saniyede log buffer'i diske yaz
        if (++loop_count >= 1000) {
            loop_count = 0;
            log_manager_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
