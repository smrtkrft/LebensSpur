/**
 * @file main.c
 * @brief LebensSpur ESP32-C6 Main Application
 * 
 * IoT Dead Man's Switch firmware.
 * 
 * Features:
 * - Unique device ID from chip MAC (LS-XXXXXXXXXXXX format)
 * - WiFi AP+STA dual mode
 * - Web-based setup and control
 * - Timer-based dead man's switch
 * - Email notifications
 * - Relay control
 * - External flash for data storage
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

// Module includes
#include "device_id.h"
#include "ext_flash.h"
#include "file_manager.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "log_manager.h"
#include "session_auth.h"
#include "web_server.h"
#include "timer_scheduler.h"
#include "mail_sender.h"
#include "relay_manager.h"
#include "button_manager.h"
#include "ota_manager.h"

static const char *TAG = "lebensspur";

#define FIRMWARE_VERSION    "0.1.0"

/* ============================================
 * CALLBACKS
 * ============================================ */

/**
 * @brief Timer trigger callback
 */
static void on_timer_trigger(void)
{
    ESP_LOGE(TAG, "DEAD MAN'S SWITCH TRIGGERED!");
    LOG_TIMER(LOG_LEVEL_CRITICAL, "Timer triggered - sending notifications");
    
    // Trigger relay if configured
    timer_config_t config;
    config_load_timer(&config);
    
    if (config.relay_trigger) {
        relay_config_t relay_cfg;
        config_load_relay(&relay_cfg);
        
        if (relay_cfg.pulse_mode) {
            relay_pulse_sequence(relay_cfg.pulse_count,
                                relay_cfg.pulse_duration_ms,
                                relay_cfg.pulse_interval_ms);
        } else {
            relay_on();
        }
    }
    
    // Send email notification
    if (mail_is_configured()) {
        mail_send_notification(
            "ALERT: Dead Man's Switch Triggered",
            "This is an automated alert from your LebensSpur device.\n\n"
            "The dead man's switch timer has expired without being reset.\n"
            "This may indicate that the user needs assistance.\n\n"
            "Please take appropriate action immediately."
        );
    }
}

/**
 * @brief Timer warning callback
 */
static void on_timer_warning(int warning_num, int minutes_remaining)
{
    ESP_LOGW(TAG, "Timer warning #%d: %d minutes remaining", warning_num, minutes_remaining);
    
    // Send warning email
    if (mail_is_configured()) {
        char subject[64];
        char body[256];
        
        snprintf(subject, sizeof(subject), 
                 "WARNING: Dead Man's Switch - %d minutes remaining", minutes_remaining);
        snprintf(body, sizeof(body),
                 "This is warning #%d from your LebensSpur device.\n\n"
                 "The dead man's switch timer will expire in %d minutes.\n"
                 "Please reset the timer to prevent triggering.\n\n"
                 "Access your device at: http://%s.local",
                 warning_num, minutes_remaining, wifi_manager_get_hostname());
        
        mail_send_notification(subject, body);
    }
}

/**
 * @brief Button event callback
 */
static void on_button_event(button_event_t event)
{
    switch (event) {
        case BUTTON_EVENT_CLICK:
            ESP_LOGI(TAG, "Button: single click - reset timer");
            timer_scheduler_reset();
            break;
            
        case BUTTON_EVENT_DOUBLE_CLICK:
            ESP_LOGI(TAG, "Button: double click - toggle relay");
            relay_toggle();
            break;
            
        case BUTTON_EVENT_TRIPLE_CLICK:
            ESP_LOGI(TAG, "Button: triple click - acknowledge trigger");
            if (timer_scheduler_is_triggered()) {
                timer_scheduler_acknowledge();
                relay_off();
            }
            break;
            
        case BUTTON_EVENT_LONG_PRESS:
            ESP_LOGW(TAG, "Button: long press - factory reset pending...");
            break;
            
        case BUTTON_EVENT_LONG_RELEASE:
            ESP_LOGW(TAG, "Button: long press release - performing factory reset!");
            config_factory_reset();
            esp_restart();
            break;
            
        default:
            break;
    }
}

/* ============================================
 * STARTUP BANNER
 * ============================================ */

static void print_banner(void)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║                                                            ║\n");
    printf("║     ██╗     ███████╗██████╗ ███████╗███╗   ██╗███████╗    ║\n");
    printf("║     ██║     ██╔════╝██╔══██╗██╔════╝████╗  ██║██╔════╝    ║\n");
    printf("║     ██║     █████╗  ██████╔╝█████╗  ██╔██╗ ██║███████╗    ║\n");
    printf("║     ██║     ██╔══╝  ██╔══██╗██╔══╝  ██║╚██╗██║╚════██║    ║\n");
    printf("║     ███████╗███████╗██████╔╝███████╗██║ ╚████║███████║    ║\n");
    printf("║     ╚══════╝╚══════╝╚═════╝ ╚══════╝╚═╝  ╚═══╝╚══════╝    ║\n");
    printf("║                     ███████╗██████╗ ██╗   ██╗██████╗      ║\n");
    printf("║                     ██╔════╝██╔══██╗██║   ██║██╔══██╗     ║\n");
    printf("║                     ███████╗██████╔╝██║   ██║██████╔╝     ║\n");
    printf("║                     ╚════██║██╔═══╝ ██║   ██║██╔══██╗     ║\n");
    printf("║                     ███████║██║     ╚██████╔╝██║  ██║     ║\n");
    printf("║                     ╚══════╝╚═╝      ╚═════╝ ╚═╝  ╚═╝     ║\n");
    printf("║                                                            ║\n");
    printf("║            IoT Dead Man's Switch - ESP32-C6               ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  Device ID: %s\n", device_id_get());
    printf("  Firmware:  v%s\n", FIRMWARE_VERSION);
    printf("  Build:     %s %s\n", __DATE__, __TIME__);
    printf("\n");
}

/* ============================================
 * SYSTEM INITIALIZATION
 * ============================================ */

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: erasing and reinitializing");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t system_init(void)
{
    esp_err_t ret;

    // Initialize NVS (required for WiFi)
    ret = init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 1. Device ID (reads chip MAC)
    ret = device_id_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Device ID init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Device ID: %s", device_id_get());

    // 2. External Flash
    ret = ext_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "External flash init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. File Manager (SPIFFS on external flash)
    ret = file_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "File manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 4. Config Manager (JSON on SPIFFS)
    ret = config_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 5. Increment boot counter
    uint32_t boot_count = config_increment_boot_count();
    ESP_LOGI(TAG, "Boot count: %lu", boot_count);

    // Print banner
    print_banner();

    // 6. Time Manager
    ret = time_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Time manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 7. Log Manager
    ret = log_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Log manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    LOG_SYSTEM(LOG_LEVEL_INFO, "System starting (boot #%lu)", boot_count);

    // 8. Relay Manager
    ret = relay_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relay manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 9. Button Manager
    ret = button_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    button_manager_set_callback(on_button_event);

    // 10. Session Auth
    ret = session_auth_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Session auth init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 11. WiFi Manager
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 12. Start AP mode (always available)
    ret = wifi_manager_start_ap();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "AP started: SSID=%s, Password=%s", device_id_get(), WIFI_AP_PASSWORD);
    ESP_LOGI(TAG, "mDNS: http://%s.local", wifi_manager_get_hostname());

    // 13. Connect to saved WiFi if available
    wifi_config_t wifi_cfg;
    config_load_wifi(&wifi_cfg);
    if (wifi_cfg.configured && wifi_cfg.ssid[0]) {
        ESP_LOGI(TAG, "Connecting to saved WiFi: %s", wifi_cfg.ssid);
        ret = wifi_manager_connect(wifi_cfg.ssid, wifi_cfg.password);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Connected! IP: %s", wifi_manager_get_sta_ip());
            LOG_NETWORK(LOG_LEVEL_INFO, "Connected to %s", wifi_cfg.ssid);
            
            // Start NTP sync
            time_manager_start_ntp();
        } else {
            ESP_LOGW(TAG, "WiFi connect failed, AP still available");
            LOG_NETWORK(LOG_LEVEL_WARN, "Failed to connect to %s", wifi_cfg.ssid);
        }
    }

    // 14. Mail Sender
    ret = mail_sender_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mail sender init failed (non-critical)");
    }

    // 15. OTA Manager
    ret = ota_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA manager init failed (non-critical)");
    }

    // 16. Timer Scheduler
    ret = timer_scheduler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Timer scheduler init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    timer_scheduler_set_trigger_cb(on_timer_trigger);
    timer_scheduler_set_warning_cb(on_timer_warning);

    // 17. Web Server (start last)
    ret = web_server_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Web server init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    LOG_SYSTEM(LOG_LEVEL_INFO, "System initialization complete");
    return ESP_OK;
}

/* ============================================
 * MAIN APPLICATION
 * ============================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "Starting LebensSpur...");

    // Initialize all modules
    esp_err_t ret = system_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "System initialization failed!");
        return;
    }

    ESP_LOGI(TAG, "System initialized successfully");

    // Check setup status
    if (!config_is_setup_completed()) {
        ESP_LOGW(TAG, "==============================================");
        ESP_LOGW(TAG, "  INITIAL SETUP REQUIRED");
        ESP_LOGW(TAG, "==============================================");
        ESP_LOGI(TAG, "WiFi: %s  Password: %s", device_id_get(), WIFI_AP_PASSWORD);
        ESP_LOGI(TAG, "URL:  http://%s.local", wifi_manager_get_hostname());
        ESP_LOGI(TAG, "      http://%s", wifi_manager_get_ap_ip());
        ESP_LOGW(TAG, "==============================================");
    } else {
        ESP_LOGI(TAG, "Setup complete - normal operation");
        
        // Log timer status
        timer_status_t timer_status;
        timer_scheduler_get_status(&timer_status);
        ESP_LOGI(TAG, "Timer state: %s", timer_scheduler_state_name(timer_status.state));
        
        char remaining[32];
        timer_scheduler_time_remaining_str(remaining, sizeof(remaining));
        ESP_LOGI(TAG, "Time remaining: %s", remaining);
    }

    // Main monitoring loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));  // Every minute
        
        // Log status
        wifi_status_t wifi_status;
        wifi_manager_get_status(&wifi_status);
        
        char uptime[32];
        time_manager_get_uptime_string(uptime, sizeof(uptime));
        
        char timer_str[32];
        timer_scheduler_time_remaining_str(timer_str, sizeof(timer_str));
        
        ESP_LOGI(TAG, "Status: uptime=%s, wifi=%s, timer=%s, relay=%s",
                 uptime,
                 wifi_status.sta_connected ? "connected" : "AP-only",
                 timer_str,
                 relay_is_on() ? "ON" : "OFF");
        
        // Cleanup expired sessions
        session_auth_cleanup();
    }
}
