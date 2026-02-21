/**
 * Button Manager - GPIO17 Buton Kontrolü
 *
 * Active LOW: Buton basıldığında GPIO=0, bırakıldığında GPIO=1 (pull-up)
 * Debounce: 50ms stabil durum gerektirir
 * Olay algılama: Basılı tutma süresine göre kısa/uzun/çok uzun basma
 */

#include "button_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "BUTTON";

// Zamanlama parametreleri
#define DEBOUNCE_MS         50
#define LONG_PRESS_MS       1000
#define VERY_LONG_PRESS_MS  3000

// Durum
static bool s_initialized = false;
static bool s_last_raw = false;        // Son ham okuma
static bool s_stable = false;          // Debounce sonrası stabil durum
static int64_t s_change_time = 0;      // Son durum değişiklik zamanı (us)
static int64_t s_press_start = 0;      // Basma başlangıç zamanı (us)
static bool s_long_fired = false;      // Uzun basma olayı gönderildi mi
static bool s_very_long_fired = false; // Çok uzun basma olayı gönderildi mi

// İstatistik
static uint32_t s_press_count = 0;
static uint32_t s_long_count = 0;
static uint32_t s_very_long_count = 0;

// Callback
static button_callback_t s_callback = NULL;

// ============================================================================
// Init / Deinit
// ============================================================================

esp_err_t button_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config başarısız: %s", esp_err_to_name(ret));
        return ret;
    }

    // Başlangıç durumunu oku
    s_stable = (gpio_get_level(BUTTON_GPIO_PIN) == 0);
    s_last_raw = s_stable;
    s_change_time = esp_timer_get_time();

    s_initialized = true;
    ESP_LOGI(TAG, "OK - GPIO%d (D7)", BUTTON_GPIO_PIN);

    return ESP_OK;
}

esp_err_t button_manager_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    s_callback = NULL;
    s_initialized = false;

    ESP_LOGI(TAG, "Kapatıldı");
    return ESP_OK;
}

// ============================================================================
// Callback & Status
// ============================================================================

void button_set_callback(button_callback_t callback)
{
    s_callback = callback;
}

bool button_is_pressed(void)
{
    return s_stable;
}

uint32_t button_get_press_duration(void)
{
    if (!s_stable || s_press_start == 0) {
        return 0;
    }
    return (uint32_t)((esp_timer_get_time() - s_press_start) / 1000);
}

// ============================================================================
// Tick - ~10ms aralıkla çağrılmalı
// ============================================================================

void button_tick(void)
{
    if (!s_initialized) return;

    int64_t now = esp_timer_get_time();
    bool raw = (gpio_get_level(BUTTON_GPIO_PIN) == 0);  // Active LOW

    // Ham durum değiştiyse debounce sayacını sıfırla
    if (raw != s_last_raw) {
        s_last_raw = raw;
        s_change_time = now;
        return;
    }

    // Debounce süresi geçmedi
    if ((now - s_change_time) < (DEBOUNCE_MS * 1000)) {
        return;
    }

    // Stabil durum değişti mi?
    if (raw != s_stable) {
        s_stable = raw;

        if (s_stable) {
            // Basıldı
            s_press_start = now;
            s_long_fired = false;
            s_very_long_fired = false;
        } else {
            // Bırakıldı - kısa basma kontrolü
            uint32_t duration = button_get_press_duration();

            if (!s_long_fired && duration < LONG_PRESS_MS) {
                s_press_count++;
                ESP_LOGI(TAG, "Kısa basma (%lums)", duration);
                if (s_callback) s_callback(BUTTON_EVENT_PRESS);
            }

            if (s_callback) s_callback(BUTTON_EVENT_RELEASE);
            s_press_start = 0;
        }
    }

    // Basılı tutuluyorsa süre kontrolü
    if (s_stable && s_press_start > 0) {
        uint32_t duration = button_get_press_duration();

        if (!s_long_fired && duration >= LONG_PRESS_MS && duration < VERY_LONG_PRESS_MS) {
            s_long_fired = true;
            s_long_count++;
            ESP_LOGI(TAG, "Uzun basma algılandı");
            if (s_callback) s_callback(BUTTON_EVENT_LONG_PRESS);
        }

        if (!s_very_long_fired && duration >= VERY_LONG_PRESS_MS) {
            s_very_long_fired = true;
            s_very_long_count++;
            ESP_LOGI(TAG, "Çok uzun basma algılandı");
            if (s_callback) s_callback(BUTTON_EVENT_VERY_LONG);
        }
    }
}

// ============================================================================
// Debug
// ============================================================================

void button_print_stats(void)
{
    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ GPIO:        %d (D7)", BUTTON_GPIO_PIN);
    ESP_LOGI(TAG, "│ Durum:       %s", s_stable ? "BASILI" : "SERBEST");
    ESP_LOGI(TAG, "│ Kısa basma:  %lu", s_press_count);
    ESP_LOGI(TAG, "│ Uzun basma:  %lu", s_long_count);
    ESP_LOGI(TAG, "│ Çok uzun:    %lu", s_very_long_count);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
