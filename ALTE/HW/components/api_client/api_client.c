#include "api_client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sys_log.h"
#include "wifi_manager.h"

static const char *TAG = "API_CLI";
#define NVS_NS "ls_api"

static api_config_t s_cfg;

/* ========== NVS ========== */

static void get_str(nvs_handle_t h, const char *key, char *out, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

esp_err_t api_client_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "wh_url", s_cfg.wh_url, sizeof(s_cfg.wh_url));
        get_str(h, "wh_method", s_cfg.wh_method, sizeof(s_cfg.wh_method));
        get_str(h, "wh_headers", s_cfg.wh_headers, sizeof(s_cfg.wh_headers));
        get_str(h, "wh_body", s_cfg.wh_body, sizeof(s_cfg.wh_body));
        get_str(h, "mq_url", s_cfg.mq_url, sizeof(s_cfg.mq_url));
        get_str(h, "mq_topic", s_cfg.mq_topic, sizeof(s_cfg.mq_topic));
        get_str(h, "mq_user", s_cfg.mq_user, sizeof(s_cfg.mq_user));
        get_str(h, "mq_pass", s_cfg.mq_pass, sizeof(s_cfg.mq_pass));
        get_str(h, "mq_payload", s_cfg.mq_payload, sizeof(s_cfg.mq_payload));
        nvs_close(h);
    }
    if (s_cfg.wh_method[0] == '\0') strcpy(s_cfg.wh_method, "POST");
    ESP_LOGI(TAG, "API istemcisi hazir (webhook=%d, mqtt=%d)",
             api_client_has_webhook(), api_client_has_mqtt());
    return ESP_OK;
}

esp_err_t api_client_get_config(api_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memcpy(cfg, &s_cfg, sizeof(s_cfg));
    return ESP_OK;
}

esp_err_t api_client_save_config(const api_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    if (s_cfg.wh_method[0] == '\0') strcpy(s_cfg.wh_method, "POST");
    nvs_set_str(h, "wh_url", s_cfg.wh_url);
    nvs_set_str(h, "wh_method", s_cfg.wh_method);
    nvs_set_str(h, "wh_headers", s_cfg.wh_headers);
    nvs_set_str(h, "wh_body", s_cfg.wh_body);
    nvs_set_str(h, "mq_url", s_cfg.mq_url);
    nvs_set_str(h, "mq_topic", s_cfg.mq_topic);
    nvs_set_str(h, "mq_user", s_cfg.mq_user);
    nvs_set_str(h, "mq_pass", s_cfg.mq_pass);
    nvs_set_str(h, "mq_payload", s_cfg.mq_payload);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "API ayarlari kaydedildi (webhook=%d, mqtt=%d)",
             api_client_has_webhook(), api_client_has_mqtt());
    return err;
}

bool api_client_has_webhook(void) { return s_cfg.wh_url[0] != '\0'; }
bool api_client_has_mqtt(void)    { return s_cfg.mq_url[0] != '\0' && s_cfg.mq_topic[0] != '\0'; }

/* ========== Sablon ========== */

static size_t put_str(char *out, size_t out_size, size_t oi, const char *src)
{
    if (!src) return 0;
    size_t avail = (oi < out_size) ? out_size - 1 - oi : 0;
    size_t n = strlen(src);
    if (n > avail) n = avail;
    memcpy(out + oi, src, n);
    return n;
}

static void expand(const char *tmpl, char *out, size_t out_size, const char *status)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!tmpl) return;

    char timebuf[24];
    time_t now = time(NULL);
    if (now > 1700000000) snprintf(timebuf, sizeof(timebuf), "%lld", (long long)now);
    else snprintf(timebuf, sizeof(timebuf), "%llu", (unsigned long long)(esp_timer_get_time() / 1000000ULL));

    const struct { const char *key; size_t klen; const char *val; } map[] = {
        { "{status}", 8, status },
        { "{time}",   6, timebuf },
        { "{device}", 8, wifi_manager_get_device_name() },
    };
    size_t oi = 0, ti = 0;
    while (tmpl[ti] && oi < out_size - 1) {
        bool matched = false;
        if (tmpl[ti] == '{') {
            for (size_t k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
                if (strncmp(tmpl + ti, map[k].key, map[k].klen) == 0) {
                    oi += put_str(out, out_size, oi, map[k].val);
                    ti += map[k].klen;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) out[oi++] = tmpl[ti++];
    }
    out[oi] = '\0';
}

/* ========== Webhook ========== */

esp_err_t api_client_send_webhook(const char *status)
{
    if (!api_client_has_webhook()) return ESP_ERR_INVALID_STATE;
    if (!status) status = "test";

    esp_http_client_method_t method = HTTP_METHOD_POST;
    if (strcmp(s_cfg.wh_method, "GET") == 0) method = HTTP_METHOD_GET;
    else if (strcmp(s_cfg.wh_method, "PUT") == 0) method = HTTP_METHOD_PUT;

    char *body = NULL;
    if (method != HTTP_METHOD_GET) {
        body = malloc(1024);
        if (!body) return ESP_ERR_NO_MEM;
        if (s_cfg.wh_body[0]) {
            expand(s_cfg.wh_body, body, 1024, status);
        } else {
            snprintf(body, 1024, "{\"event\":\"%s\",\"device\":\"%s\",\"time\":%llu}",
                     status, wifi_manager_get_device_name(),
                     (unsigned long long)time(NULL));
        }
    }

    esp_http_client_config_t cfg = {
        .url = s_cfg.wh_url,
        .method = method,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); return ESP_FAIL; }

    /* Ozel basliklar: JSON nesnesi */
    bool has_ct = false;
    if (s_cfg.wh_headers[0]) {
        cJSON *hdrs = cJSON_Parse(s_cfg.wh_headers);
        if (hdrs && cJSON_IsObject(hdrs)) {
            cJSON *it = NULL;
            cJSON_ArrayForEach(it, hdrs) {
                if (cJSON_IsString(it) && it->string && it->valuestring) {
                    esp_http_client_set_header(client, it->string, it->valuestring);
                    if (strcasecmp(it->string, "Content-Type") == 0) has_ct = true;
                }
            }
        } else {
            ESP_LOGW(TAG, "Webhook basliklari gecerli JSON degil, atlaniyor");
        }
        if (hdrs) cJSON_Delete(hdrs);
    }
    if (body) {
        if (!has_ct) esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    sys_log_mark_event(SL_EV_WEBHOOK, 0);
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Webhook hata: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Webhook HTTP %d (%s %s)", code, s_cfg.wh_method, status);
    return (code >= 200 && code < 300) ? ESP_OK : ESP_FAIL;
}

/* ========== MQTT ========== */

#define MQ_BIT_CONNECTED  BIT0
#define MQ_BIT_PUBLISHED  BIT1
#define MQ_BIT_ERROR      BIT2

static void mqtt_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    EventGroupHandle_t eg = (EventGroupHandle_t)arg;
    (void)base; (void)data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:    xEventGroupSetBits(eg, MQ_BIT_CONNECTED); break;
        case MQTT_EVENT_PUBLISHED:    xEventGroupSetBits(eg, MQ_BIT_PUBLISHED); break;
        case MQTT_EVENT_ERROR:
        case MQTT_EVENT_DISCONNECTED: xEventGroupSetBits(eg, MQ_BIT_ERROR); break;
        default: break;
    }
}

esp_err_t api_client_send_mqtt(const char *status)
{
    if (!api_client_has_mqtt()) return ESP_ERR_INVALID_STATE;
    if (!status) status = "test";

    char payload[512];
    if (s_cfg.mq_payload[0]) {
        expand(s_cfg.mq_payload, payload, sizeof(payload), status);
    } else {
        snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"device\":\"%s\"}",
                 status, wifi_manager_get_device_name());
    }

    EventGroupHandle_t eg = xEventGroupCreate();
    if (!eg) return ESP_ERR_NO_MEM;

    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = s_cfg.mq_url,
        .credentials.username = s_cfg.mq_user[0] ? s_cfg.mq_user : NULL,
        .credentials.authentication.password = s_cfg.mq_pass[0] ? s_cfg.mq_pass : NULL,
        .network.timeout_ms = 8000,
        .network.disable_auto_reconnect = true,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mcfg);
    if (!client) { vEventGroupDelete(eg); return ESP_FAIL; }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_cb, eg);

    sys_log_mark_event(SL_EV_WEBHOOK, 1);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err == ESP_OK) {
        EventBits_t b = xEventGroupWaitBits(eg, MQ_BIT_CONNECTED | MQ_BIT_ERROR, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(8000));
        if (b & MQ_BIT_CONNECTED) {
            int mid = esp_mqtt_client_publish(client, s_cfg.mq_topic, payload, 0, 1, 0);
            if (mid < 0) {
                err = ESP_FAIL;
            } else {
                b = xEventGroupWaitBits(eg, MQ_BIT_PUBLISHED | MQ_BIT_ERROR, pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(5000));
                err = (b & MQ_BIT_PUBLISHED) ? ESP_OK : ESP_ERR_TIMEOUT;
            }
        } else {
            err = (b & MQ_BIT_ERROR) ? ESP_FAIL : ESP_ERR_TIMEOUT;
        }
    }
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    vEventGroupDelete(eg);

    if (err == ESP_OK) ESP_LOGI(TAG, "MQTT yayinlandi: %s (%s)", s_cfg.mq_topic, status);
    else ESP_LOGE(TAG, "MQTT hata: %s", esp_err_to_name(err));
    return err;
}

void api_client_notify_all(const char *status)
{
    if (api_client_has_webhook()) {
        esp_err_t e = api_client_send_webhook(status);
        if (e == ESP_OK) sys_log_write("INFO", "api", "Webhook gonderildi (%s)", status);
        else sys_log_write("ERR", "api", "Webhook gonderilemedi (%s): %s", status, esp_err_to_name(e));
    }
    if (api_client_has_mqtt()) {
        esp_err_t e = api_client_send_mqtt(status);
        if (e == ESP_OK) sys_log_write("INFO", "api", "MQTT yayinlandi (%s): %s", status, s_cfg.mq_topic);
        else sys_log_write("ERR", "api", "MQTT yayinlanamadi (%s): %s", status, esp_err_to_name(e));
    }
    if (!api_client_has_webhook() && !api_client_has_mqtt()) {
        sys_log_write("WARN", "api", "API eylemi aktif ama webhook/MQTT yapilandirilmamis (%s)", status);
    }
}
