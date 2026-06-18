/**
 * LebensSpur - Output Yonetimi
 * Buton (GPIO18/D10): Basildiginda timer restart
 * Relais (GPIO19/D8): Timer dolunca tetiklenir
 */

#include "output.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "OUTPUT";

/* Pin tanimlari */
#define BUTTON_GPIO     GPIO_NUM_18     /* D10 - Active LOW (GND'ye baglanir) */
#define RELAY_GPIO      GPIO_NUM_19     /* D8  - Active HIGH */

/* Debounce suresi (ms) */
#define DEBOUNCE_MS     200

/* NVS */
#define NVS_NAMESPACE   "ls_output"
#define NVS_KEY_INV     "relay_inv"
#define NVS_KEY_DUR     "relay_dur"
#define NVS_KEY_PULSE   "relay_pulse"
#define NVS_KEY_DELAY   "relay_delay"

/* Relay yapilandirmasi */
static volatile uint16_t s_relay_duration = 0;   /* 0 = suresiz (sustained) */
static volatile uint8_t  s_relay_pulse = 0;       /* 0 = pulse kapalı */
static volatile uint8_t  s_relay_delay = 0;       /* baslanma gecikmesi */
static volatile bool     s_inverted = false;
static volatile bool     s_relay_on = false;

/* Pulse task handle (durdurmak icin) */
static TaskHandle_t s_pulse_task_handle = NULL;

/* Ters mantik icin GPIO seviyeleri */
static inline int relay_idle_level(void) { return s_inverted ? 1 : 0; }
static inline int relay_active_level(void) { return s_inverted ? 0 : 1; }
static void (*s_button_cb)(void) = NULL;
static void (*s_factory_reset_cb)(void) = NULL;
static volatile int64_t s_last_button_time = 0;

/* Button ISR -> queue -> task yapisi (ISR'de direkt callback cagirmak guvenli degil) */
static QueueHandle_t s_button_queue = NULL;

/* Buton ISR handler */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    (void)arg;
    /* Debounce kontrolu */
    int64_t now = esp_timer_get_time();
    if ((now - s_last_button_time) < (DEBOUNCE_MS * 1000)) {
        return;
    }
    s_last_button_time = now;

    /* Queue'ya sinyal gonder */
    uint8_t val = 1;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_button_queue, &val, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* Button handler task - Multi-press algilama
 * 1x = normal callback (timer restart)
 * 3x = esp_restart() (cihaz yeniden baslatma)
 * 5x = factory reset callback
 * Pencere: 3 saniye, basmalar arasi max 400ms bekleme
 */
static void button_task(void *arg)
{
    (void)arg;
    uint8_t val;
    int press_count = 0;
    int64_t first_press_time = 0;
    const int64_t WINDOW_US = 3000000;   /* 3 saniye pencere */
    const TickType_t WAIT_TICKS = pdMS_TO_TICKS(400);  /* Sonraki basma bekleme */

    while (1) {
        if (press_count == 0) {
            /* Ilk basmayı sonsuz bekle */
            if (xQueueReceive(s_button_queue, &val, portMAX_DELAY) == pdTRUE) {
                press_count = 1;
                first_press_time = esp_timer_get_time();
                ESP_LOGI(TAG, "Buton basildi (1/%d)", press_count);
            }
        } else {
            /* Sonraki basmalari 400ms bekle */
            if (xQueueReceive(s_button_queue, &val, WAIT_TICKS) == pdTRUE) {
                int64_t now = esp_timer_get_time();
                if ((now - first_press_time) < WINDOW_US) {
                    press_count++;
                    ESP_LOGI(TAG, "Buton basildi (%d)", press_count);

                    /* 5x'e ulasti -> hemen factory reset */
                    if (press_count >= 5) {
                        ESP_LOGW(TAG, "5x basma algilandi -> Factory Reset!");
                        if (s_factory_reset_cb) {
                            s_factory_reset_cb();
                        }
                        press_count = 0;
                        continue;
                    }
                } else {
                    /* Pencere disti, onceki sayaca gore aksiyon al, yeni basmayi sayac olarak baslat */
                    goto evaluate;
                }
            } else {
                /* Timeout - mevcut sayaca gore aksiyon al */
evaluate:
                if (press_count >= 5) {
                    ESP_LOGW(TAG, "5x basma -> Factory Reset!");
                    if (s_factory_reset_cb) {
                        s_factory_reset_cb();
                    }
                } else if (press_count >= 3) {
                    ESP_LOGW(TAG, "3x basma -> Restart!");
                    esp_restart();
                } else {
                    /* 1x veya 2x = normal callback */
                    ESP_LOGI(TAG, "Tek basma -> Normal callback");
                    if (s_button_cb) {
                        s_button_cb();
                    }
                }
                press_count = 0;
            }
        }
    }
}

/* Relay pulse/sustained task
 * delay -> (pulse dongusu veya sustained) -> sure dolunca kapat
 */
static void relay_worker_task(void *arg)
{
    (void)arg;

    uint16_t duration = s_relay_duration;
    uint8_t  pulse    = s_relay_pulse;
    uint8_t  delay_s  = s_relay_delay;

    /* Gecikme */
    if (delay_s > 0) {
        ESP_LOGI(TAG, "Relais gecikme: %d saniye", delay_s);
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
    }

    /* Task iptal kontrolu (delay sirasinda durdurulmus olabilir) */
    if (ulTaskNotifyTake(pdTRUE, 0)) {
        goto done;
    }

    if (pulse == 0) {
        /* Pulse kapali: surekli acik, duration sonra kapat */
        gpio_set_level(RELAY_GPIO, relay_active_level());
        s_relay_on = true;
        ESP_LOGI(TAG, "Relais acildi (sustained, dur=%d sn)", duration);

        if (duration > 0) {
            /* duration saniye bekle (her saniye iptal kontrol et) */
            for (uint16_t i = 0; i < duration; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (ulTaskNotifyTake(pdTRUE, 0)) {
                    goto done;
                }
            }
            /* Sure doldu, kapat */
            gpio_set_level(RELAY_GPIO, relay_idle_level());
            s_relay_on = false;
            ESP_LOGI(TAG, "Relais sustained sure doldu, kapatildi");
        } else {
            /* duration=0: suresiz acik, task biter ama relay acik kalir */
            ESP_LOGI(TAG, "Relais suresiz acik (sustained)");
        }
    } else {
        /* Pulse modu: acik-kapali donguleri */
        uint32_t elapsed = 0;
        uint32_t total = (duration > 0) ? (uint32_t)duration : 0xFFFFFFFF;
        int cycle = 0;

        ESP_LOGI(TAG, "Relais pulse basliyor: %d sn acik/%d sn kapali, toplam %d sn",
                 pulse, pulse, duration);

        while (elapsed < total) {
            /* Acik fazı */
            gpio_set_level(RELAY_GPIO, relay_active_level());
            s_relay_on = true;

            uint32_t on_time = pulse;
            if (duration > 0 && (elapsed + on_time) > total) {
                on_time = total - elapsed;
            }

            for (uint32_t t = 0; t < on_time; t++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (ulTaskNotifyTake(pdTRUE, 0)) {
                    goto done;
                }
            }
            elapsed += on_time;
            cycle++;

            if (elapsed >= total) break;

            /* Kapali fazı */
            gpio_set_level(RELAY_GPIO, relay_idle_level());
            s_relay_on = false;

            uint32_t off_time = pulse;
            if (duration > 0 && (elapsed + off_time) > total) {
                off_time = total - elapsed;
            }

            for (uint32_t t = 0; t < off_time; t++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (ulTaskNotifyTake(pdTRUE, 0)) {
                    goto done;
                }
            }
            elapsed += off_time;
        }

        /* Tum donguler tamamlandi */
        gpio_set_level(RELAY_GPIO, relay_idle_level());
        s_relay_on = false;
        ESP_LOGI(TAG, "Relais pulse tamamlandi: %d dongu", cycle);
    }

done:
    /* Iptal edildiyse relay'i kapat */
    if (s_relay_on) {
        gpio_set_level(RELAY_GPIO, relay_idle_level());
        s_relay_on = false;
        ESP_LOGI(TAG, "Relais worker iptal edildi, kapatildi");
    }
    s_pulse_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t output_init(void)
{
    /* GPIO19-23 ESP32-C6'da SDIO fonksiyonuna atanmis, once sifirla */
    gpio_reset_pin(RELAY_GPIO);

    /* Relais GPIO - output, baslangicta LOW (kapali) */
    gpio_config_t relay_cfg = {
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&relay_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relais GPIO yapilandirilamadi: %s", esp_err_to_name(ret));
        return ret;
    }
    /* NVS'den yapilandirma yukle (inverted dahil, GPIO level icin gerekli) */
    output_load_config();
    gpio_set_level(RELAY_GPIO, relay_idle_level());
    ESP_LOGI(TAG, "Relais GPIO%d yapilandirildi (inverted=%d, idle_level=%d)",
             RELAY_GPIO, (int)s_inverted, relay_idle_level());

    /* Buton GPIO - input, dahili pull-up, falling edge interrupt */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE    /* Falling edge = buton basildi */
    };
    ret = gpio_config(&btn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buton GPIO yapilandirilamadi: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Buton GPIO%d yapilandirildi (Active LOW, pull-up)", BUTTON_GPIO);

    /* Buton queue ve task olustur */
    s_button_queue = xQueueCreate(4, sizeof(uint8_t));
    if (!s_button_queue) {
        ESP_LOGE(TAG, "Buton queue olusturulamadi");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(button_task, "btn_task", 2048, NULL, 10, NULL);

    /* GPIO ISR service */
    ret = gpio_install_isr_service(0);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* ISR service zaten kurulu, sorun yok */
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO ISR service kurulamadi: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buton ISR eklenemedi: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Output baslatildi (Buton: GPIO%d, Relais: GPIO%d, dur=%d pulse=%d delay=%d inv=%d)",
             BUTTON_GPIO, RELAY_GPIO,
             s_relay_duration, s_relay_pulse, s_relay_delay, (int)s_inverted);
    return ESP_OK;
}

void output_set_button_callback(void (*cb)(void))
{
    s_button_cb = cb;
}

void output_set_factory_reset_callback(void (*cb)(void))
{
    s_factory_reset_cb = cb;
}

void output_set_relay_config(uint16_t duration, uint8_t pulse, uint8_t delay)
{
    s_relay_duration = duration;
    s_relay_pulse = pulse;
    s_relay_delay = delay;
}

void output_get_relay_config(uint16_t *duration, uint8_t *pulse, uint8_t *delay)
{
    if (duration) *duration = s_relay_duration;
    if (pulse) *pulse = s_relay_pulse;
    if (delay) *delay = s_relay_delay;
}

void output_set_inverted(bool inverted)
{
    s_inverted = inverted;
    /* Relay pasifse idle seviyesini hemen guncelle */
    if (!s_relay_on) {
        gpio_set_level(RELAY_GPIO, relay_idle_level());
    }
}

bool output_get_inverted(void)
{
    return s_inverted;
}

void output_relay_trigger(void)
{
    /* Onceki pulse task calisiyorsa durdur */
    if (s_pulse_task_handle) {
        xTaskNotifyGive(s_pulse_task_handle);
        /* Task'in bitmesini bekle */
        vTaskDelay(pdMS_TO_TICKS(100));
        s_pulse_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Relais tetikleniyor (dur=%d, pulse=%d, delay=%d, inv=%d)",
             s_relay_duration, s_relay_pulse, s_relay_delay, (int)s_inverted);

    /* Worker task baslat */
    xTaskCreate(relay_worker_task, "relay_wrk", 2048, NULL, 5, &s_pulse_task_handle);
}

void output_relay_off(void)
{
    /* Pulse task calisiyorsa durdur */
    if (s_pulse_task_handle) {
        xTaskNotifyGive(s_pulse_task_handle);
        vTaskDelay(pdMS_TO_TICKS(100));
        s_pulse_task_handle = NULL;
    }
    gpio_set_level(RELAY_GPIO, relay_idle_level());
    s_relay_on = false;
    ESP_LOGI(TAG, "Relais kapatildi (idle_level=%d)", relay_idle_level());
}

bool output_relay_is_on(void)
{
    return s_relay_on;
}

esp_err_t output_load_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        /* NVS'de kayit yoksa varsayilan kullan */
        s_relay_duration = 0;
        s_relay_pulse = 0;
        s_relay_delay = 0;
        s_inverted = false;
        return ESP_OK;
    }

    uint8_t inv = 0;
    nvs_get_u8(nvs, NVS_KEY_INV, &inv);
    s_inverted = (inv != 0);

    uint16_t dur = 0;
    nvs_get_u16(nvs, NVS_KEY_DUR, &dur);
    s_relay_duration = dur;

    uint8_t pulse = 0;
    nvs_get_u8(nvs, NVS_KEY_PULSE, &pulse);
    s_relay_pulse = pulse;

    uint8_t delay_v = 0;
    nvs_get_u8(nvs, NVS_KEY_DELAY, &delay_v);
    s_relay_delay = delay_v;

    nvs_close(nvs);
    ESP_LOGI(TAG, "Yapilandirma NVS'den yuklendi (dur=%d, pulse=%d, delay=%d, inv=%d)",
             s_relay_duration, s_relay_pulse, s_relay_delay, (int)s_inverted);
    return ESP_OK;
}

esp_err_t output_save_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_set_u8(nvs, NVS_KEY_INV, s_inverted ? 1 : 0);
    nvs_set_u16(nvs, NVS_KEY_DUR, s_relay_duration);
    nvs_set_u8(nvs, NVS_KEY_PULSE, s_relay_pulse);
    nvs_set_u8(nvs, NVS_KEY_DELAY, s_relay_delay);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Yapilandirma NVS'ye kaydedildi (dur=%d, pulse=%d, delay=%d, inv=%d)",
             s_relay_duration, s_relay_pulse, s_relay_delay, (int)s_inverted);
    return ESP_OK;
}
