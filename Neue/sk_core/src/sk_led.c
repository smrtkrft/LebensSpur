#include "sk_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sk_led";

static sk_led_cfg_t   s_cfg;
static sk_led_pattern_t s_pattern = SK_LED_PATTERN_OFF;
static bool           s_ready    = false;

static void set_level(bool on)
{
    int lvl = s_cfg.active_low ? (on ? 0 : 1) : (on ? 1 : 0);
    gpio_set_level(s_cfg.gpio_num, lvl);
}

static void led_task(void *arg)
{
    (void)arg;
    int phase = 0;
    for (;;) {
        switch (s_pattern) {
        case SK_LED_PATTERN_OFF:          set_level(false); vTaskDelay(pdMS_TO_TICKS(200)); break;
        case SK_LED_PATTERN_ON:           set_level(true);  vTaskDelay(pdMS_TO_TICKS(200)); break;
        case SK_LED_PATTERN_SLOW_BLINK:   set_level(phase & 1); phase++; vTaskDelay(pdMS_TO_TICKS(500)); break;
        case SK_LED_PATTERN_FAST_BLINK:   set_level(phase & 1); phase++; vTaskDelay(pdMS_TO_TICKS(100)); break;
        case SK_LED_PATTERN_HEARTBEAT:
            set_level(true);  vTaskDelay(pdMS_TO_TICKS(80));
            set_level(false); vTaskDelay(pdMS_TO_TICKS(120));
            set_level(true);  vTaskDelay(pdMS_TO_TICKS(80));
            set_level(false); vTaskDelay(pdMS_TO_TICKS(800));
            break;
        }
    }
}

esp_err_t sk_led_init(const sk_led_cfg_t *cfg)
{
    if (s_ready) return ESP_OK;
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << s_cfg.gpio_num,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gc);
    if (err != ESP_OK) return err;
    set_level(false);
    BaseType_t ok = xTaskCreate(led_task, "sk_led", 2048, NULL, 3, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    s_ready = true;
    ESP_LOGI(TAG, "LED on GPIO%d", s_cfg.gpio_num);
    return ESP_OK;
}

void sk_led_set(sk_led_pattern_t pattern) { s_pattern = pattern; }
