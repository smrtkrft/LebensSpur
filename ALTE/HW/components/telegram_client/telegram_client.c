/**
 * LebensSpur - Telegram Client
 * Telegram Bot API uzerinden mesaj gonderimi
 * NVS namespace: ls_telegram
 */

#include "telegram_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TELEGRAM";
#define NVS_NAMESPACE "ls_telegram"

static telegram_config_t s_config;
static bool s_initialized = false;

/* JSON string icindeki ozel karakterleri escape'le (" \ \n \r \t) */
static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t si = 0, di = 0;
    while (src[si] && di < dst_size - 1) {
        char c = src[si++];
        const char *esc = NULL;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default: break;
        }
        if (esc) {
            size_t el = strlen(esc);
            if (di + el > dst_size - 1) break;
            memcpy(&dst[di], esc, el);
            di += el;
        } else if ((unsigned char)c >= 0x20) {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return di;
}

esp_err_t telegram_client_init(void)
{
    memset(&s_config, 0, sizeof(s_config));

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(s_config.token);
        nvs_get_str(nvs, "token", s_config.token, &len);
        len = sizeof(s_config.chat_id);
        nvs_get_str(nvs, "chat_id", s_config.chat_id, &len);
        len = sizeof(s_config.message);
        nvs_get_str(nvs, "message", s_config.message, &len);
        nvs_close(nvs);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Telegram client baslatildi (configured=%d)", telegram_client_has_config());
    return ESP_OK;
}

esp_err_t telegram_client_save_config(const telegram_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    esp_err_t e1 = nvs_set_str(nvs, "token", cfg->token);
    esp_err_t e2 = nvs_set_str(nvs, "chat_id", cfg->chat_id);
    esp_err_t e3 = nvs_set_str(nvs, "message", cfg->message);
    esp_err_t ec = nvs_commit(nvs);
    nvs_close(nvs);

    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || ec != ESP_OK) {
        ESP_LOGE(TAG, "NVS yazim hatasi: token=%s chat=%s msg=%s commit=%s",
                 esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3), esp_err_to_name(ec));
        return ESP_FAIL;
    }

    /* RAM kopyasini guncelle */
    memcpy(&s_config, cfg, sizeof(s_config));
    ESP_LOGI(TAG, "Telegram kaydedildi (token=%dch, chat_id=%s)",
             (int)strlen(cfg->token), cfg->chat_id);
    return ESP_OK;
}

esp_err_t telegram_client_get_config(telegram_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) telegram_client_init();
    memcpy(cfg, &s_config, sizeof(s_config));
    return ESP_OK;
}

bool telegram_client_has_config(void)
{
    if (!s_initialized) telegram_client_init();
    return (strlen(s_config.token) > 0 && strlen(s_config.chat_id) > 0);
}

esp_err_t telegram_client_send(const char *text)
{
    if (!text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;
    if (!telegram_client_has_config()) {
        ESP_LOGW(TAG, "Telegram yapilandirilmamis");
        return ESP_ERR_INVALID_STATE;
    }

    /* URL: https://api.telegram.org/bot<TOKEN>/sendMessage */
    char url[180];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", s_config.token);

    /* JSON body: {"chat_id":"<ID>","text":"<TEXT>"} - ozel karakterler escape'lenir */
    char escaped_text[513];
    json_escape(text, escaped_text, sizeof(escaped_text));

    char escaped_chat[65];
    json_escape(s_config.chat_id, escaped_chat, sizeof(escaped_chat));

    size_t body_len = strlen(escaped_chat) + strlen(escaped_text) + 40;
    char *body = malloc(body_len);
    if (!body) return ESP_ERR_NO_MEM;
    snprintf(body, body_len, "{\"chat_id\":\"%s\",\"text\":\"%s\"}", escaped_chat, escaped_text);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP hatasi: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    if (status == 200) {
        ESP_LOGI(TAG, "Mesaj gonderildi (chat_id=%s)", s_config.chat_id);
        return ESP_OK;
    } else if (status == 401 || status == 403) {
        ESP_LOGE(TAG, "Auth hatasi (HTTP %d) - token yanlis", status);
        return ESP_ERR_INVALID_ARG;
    } else {
        ESP_LOGE(TAG, "Telegram API hatasi (HTTP %d)", status);
        return ESP_FAIL;
    }
}
