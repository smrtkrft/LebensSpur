/*
 * ch_http — Telegram Bot API ve webhook (esp_http_client; CA paketi, 10 sn, 2 yönlendirme, Connection: close).
 * esp_http_client_close başka görevden çağrılmaz (kilitsiz); son tarih timeout_ms ile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "cfg.h"
#include "channel.h"
#include "evlog.h"

static channel_result_t http_class(int status, esp_err_t e)
{
    if (e != ESP_OK) return CR_RETRY;
    if (status >= 200 && status < 300) return CR_OK;
    if (status == 401 || status == 403 || status == 404) return CR_CONFIG;
    if (status == 408 || status == 429 || status >= 500) return CR_RETRY;
    return CR_PERM;
}

static channel_result_t do_request(const char *url, esp_http_client_method_t m, const char *ctype,
                                   const char *headers_json, const char *body, uint16_t idem,
                                   char *resp, size_t rcap, char *err, size_t errlen)
{
    esp_http_client_config_t c = { .url = url, .method = m, .timeout_ms = 10000, .buffer_size_tx = 1024,
                                   .max_redirection_count = 2, .crt_bundle_attach = esp_crt_bundle_attach,
                                   .keep_alive_enable = false };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return (snprintf(err, errlen, "istemci"), CR_RETRY);
    esp_http_client_set_header(h, "Connection", "close");
    if (ctype) esp_http_client_set_header(h, "Content-Type", ctype);
    char idk[16];
    if (idem) { snprintf(idk, sizeof idk, "ls-%u", idem); esp_http_client_set_header(h, "Idempotency-Key", idk); }
    cJSON *hj = headers_json && headers_json[0] ? cJSON_Parse(headers_json) : NULL;
    for (cJSON *it = hj ? hj->child : NULL; it; it = it->next) {
        if (cJSON_IsString(it) && it->string) esp_http_client_set_header(h, it->string, it->valuestring);
    }
    int blen = body ? (int)strlen(body) : 0;
    esp_err_t e = esp_http_client_open(h, blen);
    if (e == ESP_OK && blen && esp_http_client_write(h, body, blen) != blen) e = ESP_FAIL;
    int status = 0;
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        int n = resp ? esp_http_client_read_response(h, resp, (int)rcap - 1) : 0;
        if (resp) resp[n > 0 ? n : 0] = 0;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    cJSON_Delete(hj);
    channel_result_t r = http_class(status, e);
    if (r != CR_OK) {
        if (e != ESP_OK) snprintf(err, errlen, "baglanti: %s", esp_err_to_name(e));
        else snprintf(err, errlen, "HTTP %d", status);
    }
    return r;
}

channel_result_t ch_telegram_send(const channel_ctx_t *ctx, char *err, size_t errlen)
{
    static cfg_telegram_t tg;
    cfg_copy_section(CFG_SEC_TELEGRAM, &tg);
    if (!tg.token[0] || !tg.chat_id[0]) return (snprintf(err, errlen, "telegram ayari yok"), CR_CONFIG);
    tmpl_vars_t v;
    char tb[32], rb[32], text[600];
    channel_vars(ctx, &v, tb, sizeof tb, rb, sizeof rb);
    const char *tpl = ctx->job->kind == JK_TEST ? "LebensSpur test OK ({device})"
                      : tg.message[0] ? tg.message : "LebensSpur {device}: {status} {k}/{n}, {remaining}";
    tmpl_expand(tpl, &v, text, sizeof text, false);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "chat_id", tg.chat_id);
    cJSON_AddStringToObject(b, "text", text);                /* parse_mode yok: kullanıcı metni biçim bozmasın */
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    char url[128];
    snprintf(url, sizeof url, "https://api.telegram.org/bot%s/sendMessage", tg.token);
    static char resp[256];
    channel_result_t r = body ? do_request(url, HTTP_METHOD_POST, "application/json", NULL, body, 0, resp,
                                           sizeof resp, err, errlen) : CR_RETRY;
    memset(url, 0, sizeof url);                              /* token içeriyor */
    cJSON_free(body);
    if (r != CR_OK) {
        cJSON *j = cJSON_Parse(resp);
        const cJSON *d = j ? cJSON_GetObjectItem(j, "description") : NULL;
        if (cJSON_IsString(d)) snprintf(err, errlen, "telegram: %.60s", d->valuestring);
        if (j && strstr(d && d->valuestring ? d->valuestring : "", "chat not found")) r = CR_CONFIG;
        cJSON_Delete(j);
    }
    return r;
}

channel_result_t ch_webhook_send(const channel_ctx_t *ctx, char *err, size_t errlen)
{
    static cfg_webhook_t wh;
    cfg_copy_section(CFG_SEC_WEBHOOK, &wh);
    if (!wh.url[0]) return (snprintf(err, errlen, "webhook ayari yok"), CR_CONFIG);
    if (strncmp(wh.url, "https://", 8) != 0 && strncmp(wh.url, "http://", 7) != 0) {
        return (snprintf(err, errlen, "gecersiz URL"), CR_CONFIG);
    }
    tmpl_vars_t v;
    char tb[32], rb[32];
    static char body[600], url[300];
    channel_vars(ctx, &v, tb, sizeof tb, rb, sizeof rb);
    const char *tpl = wh.body[0] ? wh.body : "{\"status\":\"{status}\",\"device\":\"{device}\",\"time\":\"{time}\"}";
    tmpl_expand(tpl, &v, body, sizeof body, true);
    tmpl_expand(wh.url, &v, url, sizeof url, false);
    static const esp_http_client_method_t m[] = { HTTP_METHOD_POST, HTTP_METHOD_GET, HTTP_METHOD_PUT };
    esp_http_client_method_t meth = m[wh.method < 3 ? wh.method : 0];
    return do_request(url, meth, meth == HTTP_METHOD_GET ? NULL : "application/json", wh.headers,
                      meth == HTTP_METHOD_GET ? NULL : body, ctx->trigger_seq, NULL, 0, err, errlen);
}
