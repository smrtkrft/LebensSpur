#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "sys_worker.h"

static const char *TAG = "sys_worker";
typedef struct { sys_job_fn_t fn; void *arg; } item_t;
static QueueHandle_t s_q;

static void worker(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    item_t it;
    for (;;) {
        if (xQueueReceive(s_q, &it, pdMS_TO_TICKS(1000)) == pdTRUE && it.fn) it.fn(it.arg);
        esp_task_wdt_reset();
    }
}

esp_err_t sys_worker_start(void)
{
    if (s_q) return ESP_OK;
    s_q = xQueueCreate(8, sizeof(item_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "sys_worker", 12 * 1024, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "gorev yaratilamadi");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sys_worker_post(sys_job_fn_t fn, void *arg)
{
    item_t it = { fn, arg };
    return (s_q && xQueueSend(s_q, &it, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

void sys_worker_feed(void) { esp_task_wdt_reset(); }
