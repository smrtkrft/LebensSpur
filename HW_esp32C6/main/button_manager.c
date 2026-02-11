/**
 * @file button_manager.c
 * @brief Button Input Implementation
 */

#include "button_manager.h"
#include "log_manager.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "button";

/* ============================================
 * PRIVATE DATA
 * ============================================ */

static button_event_cb_t s_callback = NULL;
static button_event_t s_last_event = BUTTON_EVENT_NONE;
static int64_t s_press_time = 0;
static int s_click_count = 0;
static int64_t s_last_click_time = 0;
static bool s_pressed = false;
static bool s_long_press_fired = false;
static TaskHandle_t s_task_handle = NULL;
static QueueHandle_t s_event_queue = NULL;

/* ============================================
 * PRIVATE FUNCTIONS
 * ============================================ */

static void fire_event(button_event_t event)
{
    s_last_event = event;
    
    ESP_LOGD(TAG, "Button event: %s", button_event_name(event));
    
    if (s_callback) {
        s_callback(event);
    }
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(s_event_queue, &gpio_num, NULL);
}

static void button_task(void *arg)
{
    uint32_t io_num;
    int64_t now;
    
    while (1) {
        // Wait for GPIO interrupt
        if (xQueueReceive(s_event_queue, &io_num, pdMS_TO_TICKS(100))) {
            // Debounce
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            
            bool level = gpio_get_level(BUTTON_GPIO);
            bool pressed = (level == 0);  // Active LOW
            
            now = esp_timer_get_time() / 1000;  // Convert to ms
            
            if (pressed && !s_pressed) {
                // Button pressed
                s_pressed = true;
                s_press_time = now;
                s_long_press_fired = false;
                fire_event(BUTTON_EVENT_PRESSED);
                
            } else if (!pressed && s_pressed) {
                // Button released
                s_pressed = false;
                uint32_t duration = (uint32_t)(now - s_press_time);
                
                if (s_long_press_fired) {
                    fire_event(BUTTON_EVENT_LONG_RELEASE);
                    s_click_count = 0;
                } else {
                    fire_event(BUTTON_EVENT_RELEASED);
                    
                    // Count clicks
                    if (now - s_last_click_time < BUTTON_MULTI_CLICK_MS) {
                        s_click_count++;
                    } else {
                        s_click_count = 1;
                    }
                    s_last_click_time = now;
                }
            }
        }
        
        // Check for long press while pressed
        if (s_pressed && !s_long_press_fired) {
            now = esp_timer_get_time() / 1000;
            if (now - s_press_time >= BUTTON_LONG_PRESS_MS) {
                s_long_press_fired = true;
                fire_event(BUTTON_EVENT_LONG_PRESS);
            }
        }
        
        // Process multi-click timeout
        if (!s_pressed && s_click_count > 0) {
            now = esp_timer_get_time() / 1000;
            if (now - s_last_click_time >= BUTTON_MULTI_CLICK_MS) {
                switch (s_click_count) {
                    case 1:
                        fire_event(BUTTON_EVENT_CLICK);
                        break;
                    case 2:
                        fire_event(BUTTON_EVENT_DOUBLE_CLICK);
                        break;
                    default:
                        fire_event(BUTTON_EVENT_TRIPLE_CLICK);
                        break;
                }
                s_click_count = 0;
            }
        }
    }
}

/* ============================================
 * INITIALIZATION
 * ============================================ */

esp_err_t button_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing button manager (GPIO%d)", BUTTON_GPIO);
    
    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create event queue
    s_event_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Queue create failed");
        return ESP_ERR_NO_MEM;
    }
    
    // Install ISR service
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, (void *)BUTTON_GPIO);
    
    // Create task
    xTaskCreate(button_task, "button_task", 2048, NULL, 10, &s_task_handle);
    if (!s_task_handle) {
        ESP_LOGE(TAG, "Task create failed");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Button manager initialized");
    return ESP_OK;
}

void button_manager_deinit(void)
{
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    
    gpio_isr_handler_remove(BUTTON_GPIO);
    gpio_reset_pin(BUTTON_GPIO);
    
    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
}

void button_manager_set_callback(button_event_cb_t cb)
{
    s_callback = cb;
}

/* ============================================
 * STATUS
 * ============================================ */

bool button_is_pressed(void)
{
    return s_pressed;
}

button_event_t button_get_last_event(void)
{
    return s_last_event;
}

uint32_t button_get_press_duration(void)
{
    if (!s_pressed) {
        return 0;
    }
    
    int64_t now = esp_timer_get_time() / 1000;
    return (uint32_t)(now - s_press_time);
}

const char* button_event_name(button_event_t event)
{
    switch (event) {
        case BUTTON_EVENT_NONE:         return "NONE";
        case BUTTON_EVENT_PRESSED:      return "PRESSED";
        case BUTTON_EVENT_RELEASED:     return "RELEASED";
        case BUTTON_EVENT_CLICK:        return "CLICK";
        case BUTTON_EVENT_DOUBLE_CLICK: return "DOUBLE_CLICK";
        case BUTTON_EVENT_TRIPLE_CLICK: return "TRIPLE_CLICK";
        case BUTTON_EVENT_LONG_PRESS:   return "LONG_PRESS";
        case BUTTON_EVENT_LONG_RELEASE: return "LONG_RELEASE";
        default:                        return "UNKNOWN";
    }
}
