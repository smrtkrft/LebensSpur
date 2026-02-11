/**
 * @file relay_manager.c
 * @brief Relay Control Implementation
 */

#include "relay_manager.h"
#include "config_manager.h"
#include "log_manager.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "relay";

static relay_state_t s_state = RELAY_OFF;
static int64_t s_on_time = 0;
static relay_config_t s_config;

/* ============================================
 * PRIVATE FUNCTIONS
 * ============================================ */

static void set_gpio(bool level)
{
    bool actual_level = s_config.inverted ? !level : level;
    gpio_set_level(RELAY_GPIO, actual_level ? 1 : 0);
}

/* ============================================
 * INITIALIZATION
 * ============================================ */

esp_err_t relay_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing relay manager (GPIO%d)", RELAY_GPIO);
    
    // Load config
    config_load_relay(&s_config);
    
    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Start with relay OFF
    set_gpio(false);
    s_state = RELAY_OFF;
    
    ESP_LOGI(TAG, "Relay manager initialized (inverted=%d)", s_config.inverted);
    return ESP_OK;
}

void relay_manager_deinit(void)
{
    relay_off();
    gpio_reset_pin(RELAY_GPIO);
}

/* ============================================
 * CONTROL
 * ============================================ */

esp_err_t relay_on(void)
{
    if (s_state == RELAY_ON) {
        return ESP_OK;
    }
    
    // Apply delay if configured
    if (s_config.on_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_config.on_delay_ms));
    }
    
    set_gpio(true);
    s_state = RELAY_ON;
    s_on_time = esp_timer_get_time();
    
    LOG_RELAY(LOG_LEVEL_INFO, "Relay ON");
    return ESP_OK;
}

esp_err_t relay_off(void)
{
    if (s_state == RELAY_OFF) {
        return ESP_OK;
    }
    
    // Apply delay if configured
    if (s_config.off_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_config.off_delay_ms));
    }
    
    set_gpio(false);
    
    uint32_t on_duration = relay_get_on_duration();
    s_state = RELAY_OFF;
    s_on_time = 0;
    
    LOG_RELAY(LOG_LEVEL_INFO, "Relay OFF (was on %lums)", on_duration);
    return ESP_OK;
}

esp_err_t relay_toggle(void)
{
    return (s_state == RELAY_ON) ? relay_off() : relay_on();
}

esp_err_t relay_set(relay_state_t state)
{
    return (state == RELAY_ON) ? relay_on() : relay_off();
}

esp_err_t relay_pulse(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        duration_ms = s_config.pulse_duration_ms;
    }
    
    ESP_LOGD(TAG, "Pulse for %lums", duration_ms);
    
    relay_on();
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    relay_off();
    
    return ESP_OK;
}

esp_err_t relay_pulse_sequence(int count, uint32_t on_ms, uint32_t off_ms)
{
    if (count <= 0) {
        count = s_config.pulse_count;
    }
    if (on_ms == 0) {
        on_ms = s_config.pulse_duration_ms;
    }
    if (off_ms == 0) {
        off_ms = s_config.pulse_interval_ms;
    }
    
    LOG_RELAY(LOG_LEVEL_INFO, "Pulse sequence: %d x %lums on, %lums off", count, on_ms, off_ms);
    
    for (int i = 0; i < count; i++) {
        relay_on();
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        relay_off();
        
        if (i < count - 1) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
    
    return ESP_OK;
}

/* ============================================
 * STATUS
 * ============================================ */

relay_state_t relay_get_state(void)
{
    return s_state;
}

bool relay_is_on(void)
{
    return s_state == RELAY_ON;
}

uint32_t relay_get_on_duration(void)
{
    if (s_state != RELAY_ON || s_on_time == 0) {
        return 0;
    }
    
    int64_t now = esp_timer_get_time();
    return (uint32_t)((now - s_on_time) / 1000);
}
