#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "io.h"

#define PIN_BUTTON  GPIO_NUM_18
#define PIN_RELAY   GPIO_NUM_19
#define DEBOUNCE_US 200000

static io_button_isr_cb_t s_cb;
static int64_t s_last_us;
static volatile bool s_inv;          /* kapanış işleyicisi boşta seviyesini buradan bilir */
static uint32_t s_boot_pad;
static bool s_boot_held;

static inline int idle_level(bool inverted) { return inverted ? 1 : 0; }

void io_relay_early_init(void)
{
    /* Polarite henüz bilinmiyor: pin sürülmez (inverted bağlantıda LOW = röle çekili olurdu).
       Önceki esp_restart'tan kalan tutma varsa pad zaten boşta seviyesinde durur. */
    s_boot_pad = REG_READ(IO_MUX_GPIO19_REG);
    s_boot_held = gpio_ll_is_digital_io_hold(&GPIO, PIN_RELAY);
}

static void relay_shutdown(void)
{
    /* esp_restart öncesi: boşta seviyesi + tutma → reset ve bootloader boyunca pin oynamaz */
    gpio_set_level(PIN_RELAY, idle_level(s_inv));
    gpio_hold_en(PIN_RELAY);
}

void io_relay_init(bool inverted)
{
    s_inv = inverted;
    gpio_set_level(PIN_RELAY, idle_level(inverted));   /* önce çıkış yazmacı */
    gpio_pullup_dis(PIN_RELAY);
    gpio_pulldown_dis(PIN_RELAY);
    gpio_set_direction(PIN_RELAY, GPIO_MODE_OUTPUT);    /* yönlendirme → OE → GPIO işlevi; seviye zaten boşta */
    gpio_hold_dis(PIN_RELAY);                           /* tutma varsa aynı seviyeye bırakılır: kenar yok */
    esp_register_shutdown_handler(relay_shutdown);
}

void io_relay_set(bool energized, bool inverted)
{
    s_inv = inverted;
    gpio_set_level(PIN_RELAY, (energized != inverted) ? 1 : 0);
}

uint32_t io_relay_boot_pad(void) { return s_boot_pad; }
bool     io_relay_boot_held(void) { return s_boot_held; }

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
