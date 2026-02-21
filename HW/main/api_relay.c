#include "api_relay.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "config_manager.h"
#include "relay_manager.h"
#include <cJSON.h>
#include <string.h>

esp_err_t h_api_relay_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    relay_status_t st;
    relay_get_status(&st);

    const char *snames[] = {"idle", "delay", "active", "pulsing"};

    char json[256];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"gpio_level\":%d,\"energy_output\":%s,"
        "\"remaining_delay\":%lu,\"remaining_duration\":%lu,"
        "\"pulse_count\":%lu,\"trigger_count\":%lu}",
        snames[st.state], st.gpio_level,
        st.energy_output ? "true" : "false",
        st.remaining_delay, st.remaining_duration,
        st.pulse_count, st.trigger_count);

    return web_server_send_json(req, json);
}

esp_err_t h_api_relay_control(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[128] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *act = cJSON_GetObjectItem(json, "action");
    const char *action = cJSON_IsString(act) ? act->valuestring : "";

    esp_err_t ret = ESP_FAIL;
    if (strcmp(action, "on") == 0) ret = relay_on();
    else if (strcmp(action, "off") == 0) ret = relay_off();
    else if (strcmp(action, "toggle") == 0) ret = relay_toggle();
    else if (strcmp(action, "trigger") == 0) ret = relay_trigger();
    else if (strcmp(action, "pulse") == 0) {
        cJSON *dur = cJSON_GetObjectItem(json, "duration_ms");
        ret = relay_pulse(cJSON_IsNumber(dur) ? (uint32_t)dur->valuedouble : 500);
    } else {
        cJSON_Delete(json);
        return web_server_send_error(req, 400, "Invalid action");
    }

    cJSON_Delete(json);
    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    return web_server_send_error(req, 500, "Relay error");
}

esp_err_t h_api_relay_test(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    if (relay_pulse(500) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Relay test failed");
}

esp_err_t h_api_config_relay_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    ls_relay_config_t cfg;
    if (config_load_relay(&cfg) != ESP_OK) {
        ls_relay_config_t def = LS_RELAY_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    char json[256];
    snprintf(json, sizeof(json),
        "{\"inverted\":%s,"
        "\"pulseMode\":%s,"
        "\"pulseDurationMs\":%lu,"
        "\"pulseIntervalMs\":%lu,"
        "\"onDelayMs\":%lu,"
        "\"offDelayMs\":%lu}",
        cfg.inverted ? "true" : "false",
        cfg.pulse_enabled ? "true" : "false",
        (unsigned long)(cfg.pulse_on_ms),
        (unsigned long)(cfg.pulse_off_ms),
        (unsigned long)(cfg.delay_seconds * 1000UL),
        (unsigned long)(cfg.duration_seconds * 1000UL));

    return web_server_send_json(req, json);
}

esp_err_t h_api_config_relay_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    ls_relay_config_t cfg;
    config_load_relay(&cfg);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "inverted"))) cfg.inverted = cJSON_IsTrue(it);

    if ((it = cJSON_GetObjectItem(json, "pulseMode"))) cfg.pulse_enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(json, "pulseDurationMs"))) cfg.pulse_on_ms = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "pulseIntervalMs"))) cfg.pulse_off_ms = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "onDelayMs"))) cfg.delay_seconds = (uint32_t)(it->valuedouble / 1000.0);
    if ((it = cJSON_GetObjectItem(json, "offDelayMs"))) cfg.duration_seconds = (uint32_t)(it->valuedouble / 1000.0);

    if ((it = cJSON_GetObjectItem(json, "delay_seconds"))) cfg.delay_seconds = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "duration_seconds"))) cfg.duration_seconds = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "pulse_enabled"))) cfg.pulse_enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(json, "pulse_on_ms"))) cfg.pulse_on_ms = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "pulse_off_ms"))) cfg.pulse_off_ms = (uint32_t)it->valuedouble;

    cJSON_Delete(json);

    if (config_save_relay(&cfg) == ESP_OK) {
        relay_config_t rcfg = {
            .inverted = cfg.inverted,
            .delay_seconds = cfg.delay_seconds,
            .duration_seconds = cfg.duration_seconds,
            .pulse_enabled = cfg.pulse_enabled,
            .pulse_on_ms = cfg.pulse_on_ms,
            .pulse_off_ms = cfg.pulse_off_ms
        };
        relay_set_config(&rcfg);
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}
