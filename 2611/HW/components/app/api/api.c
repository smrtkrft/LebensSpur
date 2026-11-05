/*
 * api — ilk uç kümesi: kimlik, durum/SSE, sayaç, WiFi, yapılandırma bölümleri.
 * Kalan uçlar (gruplar, testler, OTA, cihaz, uzaktan) adım 4–6'da ayrı dosyalarda.
 */
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "api.h"
#include "api_priv.h"
#include "cfg.h"
#include "core.h"
#include "evlog.h"
#include "net_wifi.h"
#include "storage.h"
#include "warn_sched.h"

/* ---------- durum ---------- */

static const char *timer_state_name(timer_state_t s)
{
    static const char *const n[] = { "idle", "running", "vacation", "stopped", "triggered" };
    return (unsigned)s < 5 ? n[s] : "idle";
}

static const char *wifi_state_name(wifi_state_t s)
{
    static const char *const n[] = { "ap_only", "scanning", "connecting", "connected", "disconnecting", "retry_wait" };
    return (unsigned)s < 6 ? n[s] : "ap_only";
}

cJSON *api_status_json(void)
{
    app_status_t st;
    core_get_status(&st);
    wifi_status_t w;
    net_wifi_get_status(&w);
    cJSON *o = cJSON_CreateObject();
    cJSON *t = cJSON_AddObjectToObject(o, "timer");
    cJSON_AddStringToObject(t, "state", timer_state_name(st.state));
    cJSON_AddNumberToObject(t, "total", st.total_s);
    cJSON_AddNumberToObject(t, "remaining", st.remaining_s);
    cJSON_AddNumberToObject(t, "vac_total", st.vac_total_s);
    cJSON_AddNumberToObject(t, "vac_remaining", st.vac_remaining_s);
    cJSON_AddNumberToObject(t, "warn_fired", st.warn_fired);
    cJSON *plan = cJSON_AddArrayToObject(t, "warn_plan");
    for (int i = 0; i < st.warn_n; i++) cJSON_AddItemToArray(plan, cJSON_CreateNumber(st.warn_off_s[i]));
    cJSON *rl = cJSON_AddObjectToObject(o, "relay");
    cJSON_AddBoolToObject(rl, "on", st.relay_on);
    cJSON_AddNumberToObject(rl, "remaining", st.relay_remaining_s == UINT32_MAX ? -1 : (double)st.relay_remaining_s);
    cJSON *tr = cJSON_AddObjectToObject(o, "delivery");
    cJSON_AddNumberToObject(tr, "pending", st.trigger.pending);
    cJSON_AddNumberToObject(tr, "done", st.trigger.done);
    cJSON_AddNumberToObject(tr, "failed", st.trigger.failed);
    cJSON *wf = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(wf, "state", wifi_state_name(w.state));
    cJSON_AddStringToObject(wf, "ip", w.ip);
    cJSON_AddStringToObject(wf, "ssid", w.ssid);
    cJSON_AddNumberToObject(wf, "rssi", w.rssi);
    cJSON_AddBoolToObject(wf, "ap_on", w.ap_on);
    cJSON_AddNumberToObject(wf, "ap_clients", w.ap_clients);
    cJSON_AddStringToObject(o, "storage", storage_state_name(storage_state(STG_DATA)));
    cJSON_AddBoolToObject(o, "auth", http_auth_enabled());
    cJSON_AddStringToObject(o, "device", net_wifi_device_name());
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "uptime", evlog_uptime_s());
    return o;
}

static esp_err_t h_status(http_req_t *r) { return http_ok(r, api_status_json()); }

/* ---------- kimlik ---------- */

static esp_err_t h_auth(http_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", http_auth_enabled());
    cJSON_AddBoolToObject(o, "logged_in", !http_auth_enabled() || http_auth_session_valid(r));
    cJSON_AddNumberToObject(o, "locked", http_auth_locked_s(r));
    return http_ok(r, o);
}

static esp_err_t h_login(http_req_t *r)
{
    const cJSON *p = cJSON_GetObjectItem(http_body(r), "password");
    if (!cJSON_IsString(p)) return http_fail(r, 400, "validation", "parola", "password");
    esp_err_t e = http_auth_login(r, p->valuestring);
    if (e == ESP_ERR_INVALID_STATE) return http_fail(r, 429, "locked", "", NULL);
    if (e != ESP_OK) return http_fail(r, 401, "wrong_password", "", "password");
    return http_ok(r, NULL);
}

static esp_err_t h_logout(http_req_t *r)
{
    http_auth_logout(r);
    return http_ok(r, NULL);
}

static esp_err_t h_password(http_req_t *r)
{
    const cJSON *cur = cJSON_GetObjectItem(http_body(r), "current");
    const cJSON *nxt = cJSON_GetObjectItem(http_body(r), "next");
    if (!cJSON_IsString(nxt)) return http_fail(r, 400, "validation", "yeni parola", "next");
    esp_err_t e = http_auth_set_password(cJSON_IsString(cur) ? cur->valuestring : "", nxt->valuestring);
    if (e == ESP_ERR_INVALID_ARG) return http_fail(r, 403, "wrong_password", "", "current");
    if (e == ESP_ERR_INVALID_SIZE) return http_fail(r, 400, "validation", "en az 4 karakter", "next");
    return e == ESP_OK ? http_ok(r, NULL) : http_fail(r, 500, "internal", "", NULL);
}

/* ---------- sayaç ---------- */

static uint32_t total_of(const char *unit, int value)
{
    uint32_t u = strcmp(unit, "min") == 0 ? 60u : strcmp(unit, "hour") == 0 ? 3600u : 86400u;
    return u * (uint32_t)value;
}

/* timer bölümü kaydı: alanlar + uyarı planı (alanlar arası) doğrulaması */
static esp_err_t save_timer(http_req_t *r, const cJSON *body)
{
    cfg_timer_t cur;
    cfg_copy_section(CFG_SEC_TIMER, &cur);
    static const char *const units[] = { "min", "hour", "day" };
    const cJSON *u = cJSON_GetObjectItem(body, "unit"), *v = cJSON_GetObjectItem(body, "value");
    const cJSON *n = cJSON_GetObjectItem(body, "warn_count"), *f = cJSON_GetObjectItem(body, "warn_first_s");
    const char *unit = cJSON_IsString(u) ? u->valuestring : units[cur.unit < 3 ? cur.unit : 2];
    int value = cJSON_IsNumber(v) ? v->valueint : cur.value;
    int cnt = cJSON_IsNumber(n) ? n->valueint : cur.warn_count;
    uint32_t first = cJSON_IsNumber(f) ? (uint32_t)f->valuedouble : cur.warn_first_s;
    uint32_t total = total_of(unit, value);
    warn_plan_t p = { 0 };
    if (value >= 1 && value <= 60 && cnt >= 1 && cnt <= 10) {
        warn_err_t e = warn_plan(total, (uint8_t)cnt, first, &p);
        if (e != WARN_OK) {
            char msg[48];
            snprintf(msg, sizeof msg, "bu sureye en cok %u uyari sigar", warn_max_count(total, first));
            return http_fail(r, 400, "validation", msg, e == WARN_ERR_FIRST ? "warn_first_s" : "warn_count");
        }
    }
    char err[96];
    const char *field = NULL;
    if (cfg_set_section_json(CFG_SEC_TIMER, body, err, sizeof err, &field) != ESP_OK) {
        return http_fail(r, 400, "validation", err, field);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON *pl = cJSON_AddArrayToObject(o, "warn_plan");
    for (int i = 0; i < p.n; i++) cJSON_AddItemToArray(pl, cJSON_CreateNumber(p.off_s[i]));
    return http_ok(r, o);
}

static esp_err_t h_timer_put(http_req_t *r) { return save_timer(r, http_body(r)); }

static esp_err_t post_core(http_req_t *r, core_event_t ev)
{
    return core_post(&ev) == ESP_OK ? http_ok(r, NULL) : http_fail(r, 503, "busy", "", NULL);
}

static esp_err_t h_checkin(http_req_t *r)
{
    app_status_t st;
    core_get_status(&st);
    if (st.total_s == 0) return http_fail(r, 409, "no_timer", "", NULL);
    return post_core(r, (core_event_t){ .type = EV_CHECKIN, .u.checkin.src = CHK_WEB });
}

static esp_err_t h_cancel(http_req_t *r) { return post_core(r, (core_event_t){ .type = EV_TIMER_CANCEL }); }

static esp_err_t h_vacation(http_req_t *r)
{
    const cJSON *d = cJSON_GetObjectItem(http_body(r), "days");
    if (!cJSON_IsNumber(d) || d->valueint < 1 || d->valueint > 60) return http_fail(r, 400, "validation", "1..60", "days");
    app_status_t st;
    core_get_status(&st);
    if (st.total_s == 0) return http_fail(r, 409, "no_timer", "", NULL);
    return post_core(r, (core_event_t){ .type = EV_VACATION_SET, .u.vacation.days = (uint8_t)d->valueint });
}

static esp_err_t h_vacation_del(http_req_t *r) { return post_core(r, (core_event_t){ .type = EV_VACATION_CANCEL }); }

/* ---------- yapılandırma ---------- */

static esp_err_t h_cfg_get(http_req_t *r)
{
    const cfg_section_desc_t *d = cfg_section_by_name(http_param(r, 0));
    if (!d) return http_fail(r, 404, "not_found", "bolum", NULL);
    return http_ok(r, cfg_get_section_json((cfg_section_t)(d - cfg_sections), CFG_EXPORT_API));
}

static esp_err_t h_cfg_put(http_req_t *r)
{
    const cfg_section_desc_t *d = cfg_section_by_name(http_param(r, 0));
    if (!d) return http_fail(r, 404, "not_found", "bolum", NULL);
    cfg_section_t s = (cfg_section_t)(d - cfg_sections);
    const cJSON *body = http_body(r);
    if (s == CFG_SEC_TIMER) return save_timer(r, body);
    if (s == CFG_SEC_DEVICE) {                         /* AP, STA bağlı değilken kapatılamaz */
        const cJSON *ap = cJSON_GetObjectItem(body, "ap_enabled");
        wifi_status_t w;
        net_wifi_get_status(&w);
        if (cJSON_IsFalse(ap) && w.state != WST_CONNECTED) {
            return http_fail(r, 409, "wifi_not_connected", "", "ap_enabled");
        }
    }
    char err[96];
    const char *field = NULL;
    if (cfg_set_section_json(s, body, err, sizeof err, &field) != ESP_OK) {
        return http_fail(r, 400, "validation", err, field);
    }
    if (s == CFG_SEC_WIFI_P || s == CFG_SEC_WIFI_B) net_wifi_reconnect();
    else if (s == CFG_SEC_DEVICE) net_wifi_apply_ap();
    else if (s == CFG_SEC_WIFI_TX) net_wifi_apply_tx();
    return http_ok(r, cfg_get_section_json(s, CFG_EXPORT_API));
}

/* ---------- WiFi ---------- */

static esp_err_t h_wifi(http_req_t *r)
{
    wifi_status_t w;
    net_wifi_get_status(&w);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", wifi_state_name(w.state));
    cJSON_AddStringToObject(o, "ip", w.ip);
    cJSON_AddStringToObject(o, "ssid", w.ssid);
    cJSON_AddNumberToObject(o, "rssi", w.rssi);
    cJSON_AddBoolToObject(o, "using_backup", w.using_backup);
    cJSON_AddBoolToObject(o, "ap_on", w.ap_on);
    cJSON_AddBoolToObject(o, "ap_auto_opened", w.ap_auto_opened);
    cJSON_AddNumberToObject(o, "ap_clients", w.ap_clients);
    cJSON_AddNumberToObject(o, "tx_q", w.tx_q);
    cJSON *e = cJSON_AddObjectToObject(o, "last_error");
    cJSON_AddStringToObject(e, "class", net_wifi_err_class_name(w.last_error.cls));
    cJSON_AddNumberToObject(e, "reason", w.last_error.reason);
    cJSON_AddNumberToObject(e, "rssi", w.last_error.rssi);
    cJSON_AddStringToObject(o, "device", net_wifi_device_name());
    return http_ok(r, o);
}

static esp_err_t h_wifi_connect(http_req_t *r)
{
    net_wifi_reconnect();
    return http_ok(r, NULL);
}

static esp_err_t h_wifi_delete(http_req_t *r)
{
    const char *w = http_param(r, 0);
    cfg_section_t s = strcmp(w, "p") == 0 ? CFG_SEC_WIFI_P : strcmp(w, "b") == 0 ? CFG_SEC_WIFI_B : CFG_SEC__COUNT;
    if (s == CFG_SEC__COUNT) return http_fail(r, 404, "not_found", "", NULL);
    esp_err_t e = cfg_reset_section(s);             /* parola dahil: gizli alan NVS'te kalmasın */
    if (e == ESP_OK) net_wifi_reconnect();
    return e == ESP_OK ? http_ok(r, NULL) : http_fail(r, 500, "internal", "", NULL);
}

/* ---------- tablo ---------- */

static const http_route_t s_routes[] = {
    { "/api/auth", HTTP_GET, 0, h_auth },
    { "/api/auth/login", HTTP_POST, HR_CSRF | HR_BODY, h_login },
    { "/api/auth/logout", HTTP_POST, HR_CSRF, h_logout },
    { "/api/auth/password", HTTP_PUT, HR_WRITE, h_password },
    { "/api/status", HTTP_GET, HR_AUTH, h_status },
    { "/api/events", HTTP_GET, HR_AUTH, http_sse_handler },
    { "/api/timer", HTTP_PUT, HR_WRITE, h_timer_put },
    { "/api/timer/checkin", HTTP_POST, HR_AUTH | HR_CSRF, h_checkin },
    { "/api/timer/cancel", HTTP_POST, HR_AUTH | HR_CSRF, h_cancel },
    { "/api/timer/vacation", HTTP_POST, HR_WRITE, h_vacation },
    { "/api/timer/vacation", HTTP_DELETE, HR_AUTH | HR_CSRF, h_vacation_del },
    { "/api/config/{}", HTTP_GET, HR_AUTH, h_cfg_get },
    { "/api/config/{}", HTTP_PUT, HR_WRITE, h_cfg_put },
    { "/api/wifi", HTTP_GET, HR_AUTH, h_wifi },
    { "/api/wifi/connect", HTTP_POST, HR_AUTH | HR_CSRF, h_wifi_connect },
    { "/api/wifi/{}", HTTP_DELETE, HR_AUTH | HR_CSRF, h_wifi_delete },
};

/* alt tablolar tek tabloda birleştirilir (http katmanı tek dizi bekler) */
static http_route_t s_all[sizeof s_routes / sizeof s_routes[0] + 16];

const http_route_t *api_routes(size_t *count)
{
    size_t n = 0;
    for (size_t i = 0; i < sizeof s_routes / sizeof s_routes[0]; i++) s_all[n++] = s_routes[i];
    for (size_t i = 0; i < api_group_routes_n && n < sizeof s_all / sizeof s_all[0]; i++) s_all[n++] = api_group_routes[i];
    *count = n;
    return s_all;
}
