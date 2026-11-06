#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "cfg.h"
#include "channel.h"
#include "core.h"
#include "delivery.h"
#include "evlog.h"
#include "jobs.h"

typedef struct { ob_job_t job; uint16_t trig_seq; uint32_t remaining_s; uint8_t warn_n; } item_t;
static QueueHandle_t s_q;

static const char *ch_name(uint8_t ch)
{
    static const char *const n[] = { "mail", "telegram", "webhook", "mqtt" };
    return ch < 4 ? n[ch] : "?";
}

static const char *res_name(channel_result_t r)
{
    static const char *const n[] = { "gonderildi", "yeniden denenecek", "kalici hata", "ayar hatasi" };
    return (unsigned)r < 4 ? n[r] : "?";
}

/* testi geçen kanal: o kanal için hiçbir eylem açık değilse erken uyarı ve tetiklemede aç */
static void auto_enable(uint8_t ch)
{
    cfg_actions_t a;
    cfg_copy_section(CFG_SEC_ACTIONS, &a);
    static const char *const keys[4][2] = { { "early_mail", "trig_mail" }, { "early_telegram", NULL },
                                            { "early_webhook", "trig_webhook" }, { "early_mqtt", "trig_mqtt" } };
    bool any = ch == CH_SMTP ? (a.early_mail || a.trig_mail) : ch == CH_TELEGRAM ? a.early_telegram
             : ch == CH_WEBHOOK ? (a.early_webhook || a.trig_webhook) : (a.early_mqtt || a.trig_mqtt);
    if (any || ch >= 4) return;
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < 2; i++) if (keys[ch][i]) cJSON_AddBoolToObject(o, keys[ch][i], true);
    if (cfg_set_section_json(CFG_SEC_ACTIONS, o, NULL, 0, NULL) == ESP_OK) {
        evlog_write(EVL_INFO, "api", "%s testi basarili: kanal otomatik acildi", ch_name(ch));
    }
    cJSON_Delete(o);
}

static void task(void *arg)
{
    (void)arg;
    item_t it;
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) continue;
        channel_ctx_t c = { .job = &it.job, .trigger_seq = it.trig_seq, .remaining_s = it.remaining_s,
                            .warn_n = it.warn_n };
        char err[96] = "";
        if (it.job.kind == JK_TEST) jobs_update(it.job.ext_id, JOB_RUNNING, -1, NULL);
        channel_result_t r = channel_send(&c, err, sizeof err);
        static const char *const kinds[] = { "tetik", "erken uyari", "test" };
        char who[16] = "";
        if (it.job.ch == CH_SMTP && it.job.group != OUTBOX_OWNER) snprintf(who, sizeof who, "grup %u ", it.job.group + 1);
        evlog_write(r == CR_OK ? EVL_INFO : EVL_WARN, ch_name(it.job.ch), "%s %s%s (deneme %u)%s%s",
                    kinds[it.job.kind < 3 ? it.job.kind : 2], who, res_name(r), it.job.attempt + 1,
                    err[0] ? ": " : "", err);
        if (it.job.kind == JK_TEST) {
            jobs_update(it.job.ext_id, r == CR_OK ? JOB_DONE : JOB_FAILED, 100, r == CR_OK ? "ok" : err);
            if (r == CR_OK) auto_enable(it.job.ch);
        }
        core_event_t ev = { .type = EV_DELIVERY_RESULT, .u.delivery = { .seq = it.job.seq, .result = (uint8_t)r } };
        strlcpy(ev.u.delivery.err, err, sizeof ev.u.delivery.err);
        while (core_post(&ev) != ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));   /* sonuç kaybolmasın */
    }
}

static bool dispatch(const ob_job_t *job, uint16_t trig_seq, uint32_t remaining_s, uint8_t warn_n)
{
    item_t it = { .job = *job, .trig_seq = trig_seq, .remaining_s = remaining_s, .warn_n = warn_n };
    return xQueueSend(s_q, &it, 0) == pdTRUE;
}

esp_err_t delivery_start(void)
{
    s_q = xQueueCreate(1, sizeof(item_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreate(task, "delivery", 10 * 1024, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    core_set_dispatch(dispatch);
    return ESP_OK;
}
