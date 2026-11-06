#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "io.h"

#define PIN_BUTTON  GPIO_NUM_18
#define PIN_RELAY   GPIO_NUM_19
#define DEBOUNCE_US 200000

static io_button_isr_cb_t s_cb;
static int64_t s_last_us;

void io_relay_early_init(void)
{
    gpio_config_t c = { .pin_bit_mask = 1ULL << PIN_RELAY, .mode = GPIO_MODE_OUTPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                        .intr_type = GPIO_INTR_DISABLE };
    gpio_set_level(PIN_RELAY, 0);        /* önce seviye, sonra çıkış: ara durum yok */
    gpio_config(&c);
    gpio_set_level(PIN_RELAY, 0);
}

void io_relay_set(bool energized, bool inverted)
{
    gpio_set_level(PIN_RELAY, (energized != inverted) ? 1 : 0);
}

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < DEBOUNCE_US) return;
    s_last_us = now;
    if (s_cb) s_cb();
}

esp_err_t io_button_init(io_button_isr_cb_t cb)
{
    s_cb = cb;
    gpio_config_t c = { .pin_bit_mask = 1ULL << PIN_BUTTON, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                        .intr_type = GPIO_INTR_NEGEDGE };
    esp_err_t r = gpio_config(&c);
    if (r != ESP_OK) return r;
    r = gpio_install_isr_service(0);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;
    return gpio_isr_handler_add(PIN_BUTTON, button_isr, NULL);
}
