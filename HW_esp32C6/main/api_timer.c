#include "api_timer.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "config_manager.h"
#include "timer_scheduler.h"
#include <cJSON.h>
#include <string.h>

esp_err_t h_api_timer_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    timer_status_t st;
    if (timer_get_status(&st) != ESP_OK) {
        return web_server_send_error(req, 500, "Timer error");
    }

    const char *state_names[] = {"DISABLED", "RUNNING", "WARNING", "TRIGGERED", "PAUSED"};
    const char *state_str = (st.state <= TIMER_STATE_PAUSED) ? state_names[st.state] : "DISABLED";

    timer_config_t cfg;
    if (config_load_timer(&cfg) != ESP_OK) {
        timer_config_t def = TIMER_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    char json[640];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\","
        "\"timeRemainingMs\":%llu,"
        "\"intervalMinutes\":%lu,"
        "\"warningsSent\":%lu,"
        "\"resetCount\":%lu,"
        "\"triggerCount\":%lu,"
        "\"enabled\":%s,"
        "\"vacationEnabled\":false,"
        "\"vacationDays\":0}",
        state_str,
        (unsigned long long)st.remaining_seconds * 1000ULL,
        (unsigned long)(cfg.interval_hours * 60),
        st.warning_count,
        st.reset_count,
        st.trigger_count,
        cfg.enabled ? "true" : "false");

    return web_server_send_json(req, json);
}

esp_err_t h_api_timer_reset(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    if (timer_reset() == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Timer reset failed");
}

esp_err_t h_api_config_timer_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    timer_config_t cfg;
    if (config_load_timer(&cfg) != ESP_OK) {
        timer_config_t def = TIMER_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    uint32_t interval_min = cfg.interval_hours * 60;
    uint32_t alarm_count = 0;
    if (cfg.warning_minutes > 0 && interval_min > 0) {
        alarm_count = cfg.warning_minutes / (interval_min > 60 ? 60 : 1);
        if (alarm_count == 0) alarm_count = 1;
    }

    char json[256];
    snprintf(json, sizeof(json),
        "{\"intervalMinutes\":%lu,"
        "\"alarmCount\":%lu,"
        "\"vacationEnabled\":false,"
        "\"vacationDays\":7}",
        (unsigned long)interval_min,
        (unsigned long)alarm_count);

    return web_server_send_json(req, json);
}

esp_err_t h_api_config_timer_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    timer_config_t cfg;
    config_load_timer(&cfg);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "enabled"))) cfg.enabled = cJSON_IsTrue(it);

    if ((it = cJSON_GetObjectItem(json, "intervalMinutes"))) {
        cfg.interval_hours = (uint32_t)(it->valuedouble / 60.0);
        if (cfg.interval_hours == 0) cfg.interval_hours = 1;
    }
    if ((it = cJSON_GetObjectItem(json, "interval_hours"))) cfg.interval_hours = it->valueint;

    if ((it = cJSON_GetObjectItem(json, "warningMinutes"))) cfg.warning_minutes = it->valueint;
    if ((it = cJSON_GetObjectItem(json, "warning_minutes"))) cfg.warning_minutes = it->valueint;

    if ((it = cJSON_GetObjectItem(json, "check_start")) && cJSON_IsString(it))
        strncpy(cfg.check_start, it->valuestring, sizeof(cfg.check_start) - 1);
    if ((it = cJSON_GetObjectItem(json, "check_end")) && cJSON_IsString(it))
        strncpy(cfg.check_end, it->valuestring, sizeof(cfg.check_end) - 1);
    if ((it = cJSON_GetObjectItem(json, "relay_action")) && cJSON_IsString(it))
        strncpy(cfg.relay_action, it->valuestring, sizeof(cfg.relay_action) - 1);

    cJSON_Delete(json);

    if (config_save_timer(&cfg) == ESP_OK) {
        timer_set_enabled(cfg.enabled);
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

esp_err_t h_api_timer_enable(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    if (timer_set_enabled(true) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Timer enable failed");
}

esp_err_t h_api_timer_disable(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    if (timer_set_enabled(false) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Timer disable failed");
}

esp_err_t h_api_timer_acknowledge(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    if (timer_reset() == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Timer acknowledge failed");
}

esp_err_t h_api_timer_vacation(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    cJSON *days = cJSON_GetObjectItem(json, "days");
    (void)days;

    timer_config_t cfg;
    config_load_timer(&cfg);

    if (cJSON_IsTrue(enabled)) {
        cfg.enabled = false;
    } else {
        cfg.enabled = true;
    }

    cJSON_Delete(json);

    if (config_save_timer(&cfg) == ESP_OK) {
        timer_set_enabled(cfg.enabled);
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}
