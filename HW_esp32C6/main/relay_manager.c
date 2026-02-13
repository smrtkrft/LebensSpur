/**
 * Relay Manager - GPIO18 Röle Kontrolü
 *
 * Dahili zamanlama: relay_tick() herhangi bir frekansta çağrılabilir,
 * delay/duration sayaçları esp_timer ile doğru ölçülür.
 * Pulse modu FreeRTOS one-shot timer ile yönetilir.
 */

#include "relay_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "RELAY";

// Durum
static relay_config_t s_config = RELAY_CONFIG_DEFAULT();
static relay_state_t s_state = RELAY_STATE_IDLE;
static bool s_gpio_level = false;
static bool s_initialized = false;

// Dahili zamanlama (mikrosaniye hassasiyetinde)
static int64_t s_last_tick_us = 0;
static uint32_t s_remaining_delay = 0;
static uint32_t s_remaining_duration = 0;

// Pulse
static TimerHandle_t s_pulse_timer = NULL;
static bool s_pulse_phase = false;

// İstatistik
static uint32_t s_pulse_count = 0;
static uint32_t s_trigger_count = 0;

// Forward declarations
static void set_gpio(bool level);
static void pulse_timer_cb(TimerHandle_t xTimer);

// ============================================================================
// GPIO Kontrolü
// ============================================================================

static void set_gpio(bool level)
{
    s_gpio_level = level;
    bool physical = s_config.inverted ? !level : level;
    gpio_set_level(RELAY_GPIO_PIN, physical ? 1 : 0);
}

// ============================================================================
// Init / Deinit
// ============================================================================

esp_err_t relay_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // GPIO yapılandırması
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << RELAY_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config başarısız: %s", esp_err_to_name(ret));
        return ret;
    }

    set_gpio(false);

    // Pulse timer (one-shot, gerektiğinde başlatılacak)
    s_pulse_timer = xTimerCreate("relay_pulse", pdMS_TO_TICKS(500),
                                  pdFALSE, NULL, pulse_timer_cb);
    if (!s_pulse_timer) {
        ESP_LOGE(TAG, "Pulse timer oluşturulamadı");
        return ESP_FAIL;
    }

    s_last_tick_us = esp_timer_get_time();
    s_initialized = true;
    ESP_LOGI(TAG, "OK - GPIO%d (D10)", RELAY_GPIO_PIN);

    return ESP_OK;
}

esp_err_t relay_manager_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    relay_off();

    if (s_pulse_timer) {
        xTimerDelete(s_pulse_timer, portMAX_DELAY);
        s_pulse_timer = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Kapatıldı");
    return ESP_OK;
}

// ============================================================================
// Config
// ============================================================================

esp_err_t relay_set_config(const relay_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    memcpy(&s_config, config, sizeof(relay_config_t));
    ESP_LOGI(TAG, "Config: inv=%d delay=%lus dur=%lus pulse=%d(%lums/%lums)",
             s_config.inverted, s_config.delay_seconds,
             s_config.duration_seconds, s_config.pulse_enabled,
             s_config.pulse_on_ms, s_config.pulse_off_ms);

    return ESP_OK;
}

esp_err_t relay_get_config(relay_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memcpy(config, &s_config, sizeof(relay_config_t));
    return ESP_OK;
}

// ============================================================================
// Kontrol Fonksiyonları
// ============================================================================

esp_err_t relay_trigger(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    s_trigger_count++;
    ESP_LOGI(TAG, "Tetikleme #%lu", s_trigger_count);

    // Delay varsa bekle
    if (s_config.delay_seconds > 0) {
        s_state = RELAY_STATE_DELAY;
        s_remaining_delay = s_config.delay_seconds;
        s_last_tick_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Gecikme: %lu saniye", s_remaining_delay);
        return ESP_OK;
    }

    return relay_on();
}

esp_err_t relay_on(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    // Pulse modu aktifse pulse başlat
    if (s_config.pulse_enabled) {
        return relay_start_pulsing();
    }

    set_gpio(true);
    s_state = RELAY_STATE_ACTIVE;
    s_remaining_delay = 0;

    if (s_config.duration_seconds > 0) {
        s_remaining_duration = s_config.duration_seconds;
        s_last_tick_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Açık - %lu saniye", s_remaining_duration);
    } else {
        s_remaining_duration = 0;
        ESP_LOGI(TAG, "Açık - süresiz");
    }

    return ESP_OK;
}

esp_err_t relay_off(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    relay_stop_pulsing();
    set_gpio(false);
    s_state = RELAY_STATE_IDLE;
    s_remaining_delay = 0;
    s_remaining_duration = 0;

    ESP_LOGI(TAG, "Kapalı");
    return ESP_OK;
}

esp_err_t relay_toggle(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return (s_state == RELAY_STATE_IDLE) ? relay_on() : relay_off();
}

esp_err_t relay_pulse(uint32_t duration_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    set_gpio(true);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    set_gpio(false);

    s_pulse_count++;
    ESP_LOGI(TAG, "Pulse: %lums", duration_ms);
    return ESP_OK;
}

// ============================================================================
// Pulse Modu
// ============================================================================

static void pulse_timer_cb(TimerHandle_t xTimer)
{
    if (s_state != RELAY_STATE_PULSING) return;

    s_pulse_phase = !s_pulse_phase;
    set_gpio(s_pulse_phase);

    uint32_t next_ms = s_pulse_phase ? s_config.pulse_on_ms : s_config.pulse_off_ms;
    xTimerChangePeriod(s_pulse_timer, pdMS_TO_TICKS(next_ms), 0);
    xTimerStart(s_pulse_timer, 0);

    if (s_pulse_phase) {
        s_pulse_count++;
    }
}

esp_err_t relay_start_pulsing(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    s_state = RELAY_STATE_PULSING;
    s_pulse_phase = true;
    s_remaining_delay = 0;
    set_gpio(true);
    s_pulse_count++;

    // İlk ON periyodu sonrası OFF'a geçmek için timer başlat
    xTimerChangePeriod(s_pulse_timer, pdMS_TO_TICKS(s_config.pulse_on_ms), 0);
    xTimerStart(s_pulse_timer, 0);

    if (s_config.duration_seconds > 0) {
        s_remaining_duration = s_config.duration_seconds;
        s_last_tick_us = esp_timer_get_time();
    }

    ESP_LOGI(TAG, "Pulse başladı: %lums ON / %lums OFF",
             s_config.pulse_on_ms, s_config.pulse_off_ms);
    return ESP_OK;
}

esp_err_t relay_stop_pulsing(void)
{
    if (s_pulse_timer) {
        xTimerStop(s_pulse_timer, 0);
    }
    s_pulse_phase = false;
    return ESP_OK;
}

// ============================================================================
// Status
// ============================================================================

esp_err_t relay_get_status(relay_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;

    status->state = s_state;
    status->gpio_level = s_gpio_level;
    status->energy_output = s_gpio_level;
    status->remaining_delay = s_remaining_delay;
    status->remaining_duration = s_remaining_duration;
    status->pulse_count = s_pulse_count;
    status->trigger_count = s_trigger_count;

    return ESP_OK;
}

bool relay_get_energy_output(void)
{
    return s_gpio_level;
}

bool relay_get_gpio_level(void)
{
    return gpio_get_level(RELAY_GPIO_PIN);
}

// ============================================================================
// Tick - Dahili zamanlama ile çalışır
// ============================================================================

void relay_tick(void)
{
    if (!s_initialized) return;
    if (s_state == RELAY_STATE_IDLE) return;

    // Geçen süreyi hesapla
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - s_last_tick_us;

    // Henüz 1 saniye geçmediyse çık
    if (elapsed_us < 1000000) return;

    // Kaç saniye geçtiğini hesapla (genelde 1, ama gecikmeler olabilir)
    uint32_t elapsed_sec = (uint32_t)(elapsed_us / 1000000);
    s_last_tick_us = now_us;

    switch (s_state) {
        case RELAY_STATE_DELAY:
            if (s_remaining_delay > 0) {
                if (elapsed_sec >= s_remaining_delay) {
                    s_remaining_delay = 0;
                    ESP_LOGI(TAG, "Gecikme tamamlandı, aktifleştiriliyor");
                    relay_on();
                } else {
                    s_remaining_delay -= elapsed_sec;
                }
            }
            break;

        case RELAY_STATE_ACTIVE:
        case RELAY_STATE_PULSING:
            if (s_remaining_duration > 0) {
                if (elapsed_sec >= s_remaining_duration) {
                    s_remaining_duration = 0;
                    ESP_LOGI(TAG, "Süre doldu, kapatılıyor");
                    relay_off();
                } else {
                    s_remaining_duration -= elapsed_sec;
                }
            }
            break;

        default:
            break;
    }
}

// ============================================================================
// Debug
// ============================================================================

void relay_print_stats(void)
{
    static const char *state_names[] = {"IDLE", "DELAY", "ACTIVE", "PULSING"};

    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ Durum:       %s", state_names[s_state]);
    ESP_LOGI(TAG, "│ GPIO:        %d (fiziksel)", relay_get_gpio_level());
    ESP_LOGI(TAG, "│ Enerji:      %s", s_gpio_level ? "AÇIK" : "KAPALI");
    ESP_LOGI(TAG, "│ Inverted:    %s", s_config.inverted ? "Evet" : "Hayır");
    ESP_LOGI(TAG, "│ Gecikme:     %lu sn", s_config.delay_seconds);
    ESP_LOGI(TAG, "│ Süre:        %lu sn", s_config.duration_seconds);
    ESP_LOGI(TAG, "│ Pulse:       %s (%lums/%lums)",
             s_config.pulse_enabled ? "Aktif" : "Pasif",
             s_config.pulse_on_ms, s_config.pulse_off_ms);
    ESP_LOGI(TAG, "│ Tetikleme:   %lu", s_trigger_count);
    ESP_LOGI(TAG, "│ Pulse sayısı:%lu", s_pulse_count);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
