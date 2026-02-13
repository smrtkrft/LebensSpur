#include "api_config.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "config_manager.h"
#include "file_manager.h"
#include "esp_random.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Security Config API
// ============================================================================

esp_err_t h_api_config_security_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    auth_config_t auth;
    if (config_load_auth(&auth) != ESP_OK) {
        auth_config_t def = AUTH_CONFIG_DEFAULT();
        memcpy(&auth, &def, sizeof(auth));
    }

    api_config_t api;
    if (config_load_api(&api) != ESP_OK) {
        api_config_t def = API_CONFIG_DEFAULT();
        memcpy(&api, &def, sizeof(api));
    }

    char json[512];
    snprintf(json, sizeof(json),
        "{\"loginProtection\":true,"
        "\"lockoutTime\":15,"
        "\"resetApiEnabled\":%s,"
        "\"apiKey\":\"%s\","
        "\"sessionTimeoutMin\":%lu}",
        api.enabled ? "true" : "false",
        api.token[0] ? api.token : "",
        (unsigned long)auth.session_timeout_min);

    return web_server_send_json(req, json);
}

esp_err_t h_api_config_security_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    auth_config_t auth;
    config_load_auth(&auth);
    api_config_t api;
    config_load_api(&api);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "resetApiEnabled")))
        api.enabled = cJSON_IsTrue(it);
    if ((it = cJSON_GetObjectItem(json, "sessionTimeoutMin")))
        auth.session_timeout_min = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "session_timeout_min")))
        auth.session_timeout_min = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(json, "api_enabled")))
        api.enabled = cJSON_IsTrue(it);

    cJSON_Delete(json);

    bool ok = true;
    if (config_save_auth(&auth) != ESP_OK) ok = false;
    if (config_save_api(&api) != ESP_OK) ok = false;

    if (ok) return web_server_send_json(req, "{\"success\":true}");
    return web_server_send_error(req, 500, "Save failed");
}

esp_err_t h_api_config_security_apikey(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    api_config_t api;
    if (config_load_api(&api) != ESP_OK) {
        api_config_t def = API_CONFIG_DEFAULT();
        memcpy(&api, &def, sizeof(api));
    }

    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));

    char key[33];
    for (int i = 0; i < 16; i++) {
        snprintf(&key[i * 2], 3, "%02x", rnd[i]);
    }
    key[32] = '\0';

    strncpy(api.token, key, MAX_TOKEN_LEN - 1);
    api.token[MAX_TOKEN_LEN - 1] = '\0';

    if (config_save_api(&api) == ESP_OK) {
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"success\":true,\"apiKey\":\"%s\"}", key);
        return web_server_send_json(req, resp);
    }
    return web_server_send_error(req, 500, "Save failed");
}

// ============================================================================
// Action Config API (stub - persists to LittleFS)
// ============================================================================

esp_err_t h_api_config_webhook(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[1024] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (str) {
        file_manager_write("/ext/config/webhook.json", str, strlen(str));
        free(str);
    }

    return web_server_send_json(req, "{\"success\":true}");
}

esp_err_t h_api_config_telegram(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[1024] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (str) {
        file_manager_write("/ext/config/telegram.json", str, strlen(str));
        free(str);
    }

    return web_server_send_json(req, "{\"success\":true}");
}

esp_err_t h_api_config_early_mail(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[1024] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (str) {
        file_manager_write("/ext/config/early_mail.json", str, strlen(str));
        free(str);
    }

    return web_server_send_json(req, "{\"success\":true}");
}

// ============================================================================
// Config Export/Import API
// ============================================================================

esp_err_t h_api_config_export(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    cJSON *root = cJSON_CreateObject();
    if (!root) return web_server_send_error(req, 500, "No memory");

    timer_config_t timer;
    if (config_load_timer(&timer) == ESP_OK) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddBoolToObject(t, "enabled", timer.enabled);
        cJSON_AddNumberToObject(t, "interval_hours", timer.interval_hours);
        cJSON_AddNumberToObject(t, "warning_minutes", timer.warning_minutes);
        cJSON_AddStringToObject(t, "check_start", timer.check_start);
        cJSON_AddStringToObject(t, "check_end", timer.check_end);
        cJSON_AddStringToObject(t, "relay_action", timer.relay_action);
        cJSON_AddItemToObject(root, "timer", t);
    }

    ls_wifi_config_t wifi;
    if (config_load_wifi(&wifi) == ESP_OK) {
        cJSON *w = cJSON_CreateObject();
        cJSON_AddStringToObject(w, "primary_ssid", wifi.primary_ssid);
        cJSON_AddStringToObject(w, "primary_password", wifi.primary_password);
        cJSON_AddStringToObject(w, "secondary_ssid", wifi.secondary_ssid);
        cJSON_AddStringToObject(w, "secondary_password", wifi.secondary_password);
        cJSON_AddBoolToObject(w, "ap_mode_enabled", wifi.ap_mode_enabled);
        cJSON_AddItemToObject(root, "wifi", w);
    }

    mail_config_t mail;
    if (config_load_mail(&mail) == ESP_OK) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "server", mail.server);
        cJSON_AddNumberToObject(m, "port", mail.port);
        cJSON_AddStringToObject(m, "username", mail.username);
        cJSON_AddStringToObject(m, "password", mail.password);
        cJSON_AddStringToObject(m, "sender_name", mail.sender_name);
        cJSON_AddItemToObject(root, "mail", m);
    }

    cJSON *groups = cJSON_CreateArray();
    for (int i = 0; i < MAX_MAIL_GROUPS; i++) {
        mail_group_t grp;
        if (config_load_mail_group(i, &grp) == ESP_OK) {
            cJSON *g = cJSON_CreateObject();
            cJSON_AddStringToObject(g, "name", grp.name);
            cJSON_AddBoolToObject(g, "enabled", grp.enabled);
            cJSON_AddNumberToObject(g, "recipient_count", grp.recipient_count);
            cJSON *recips = cJSON_CreateArray();
            for (int r = 0; r < grp.recipient_count && r < MAX_RECIPIENTS; r++) {
                cJSON_AddItemToArray(recips, cJSON_CreateString(grp.recipients[r]));
            }
            cJSON_AddItemToObject(g, "recipients", recips);
            cJSON_AddItemToArray(groups, g);
        }
    }
    cJSON_AddItemToObject(root, "mail_groups", groups);

    ls_relay_config_t relay;
    if (config_load_relay(&relay) == ESP_OK) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "inverted", relay.inverted);
        cJSON_AddNumberToObject(r, "delay_seconds", relay.delay_seconds);
        cJSON_AddNumberToObject(r, "duration_seconds", relay.duration_seconds);
        cJSON_AddBoolToObject(r, "pulse_enabled", relay.pulse_enabled);
        cJSON_AddNumberToObject(r, "pulse_on_ms", relay.pulse_on_ms);
        cJSON_AddNumberToObject(r, "pulse_off_ms", relay.pulse_off_ms);
        cJSON_AddItemToObject(root, "relay", r);
    }

    api_config_t api;
    if (config_load_api(&api) == ESP_OK) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddBoolToObject(a, "enabled", api.enabled);
        cJSON_AddStringToObject(a, "endpoint", api.endpoint);
        cJSON_AddBoolToObject(a, "require_token", api.require_token);
        cJSON_AddStringToObject(a, "token", api.token);
        cJSON_AddItemToObject(root, "api", a);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

esp_err_t h_api_config_import(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char *body = malloc(4096);
    if (!body) return web_server_send_error(req, 500, "No memory");

    if (read_body(req, body, 4096) < 0) {
        free(body);
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *it, *sub;

    sub = cJSON_GetObjectItem(root, "timer");
    if (cJSON_IsObject(sub)) {
        timer_config_t cfg;
        config_load_timer(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "enabled"))) cfg.enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "interval_hours"))) cfg.interval_hours = it->valueint;
        if ((it = cJSON_GetObjectItem(sub, "warning_minutes"))) cfg.warning_minutes = it->valueint;
        if ((it = cJSON_GetObjectItem(sub, "check_start")) && cJSON_IsString(it))
            strncpy(cfg.check_start, it->valuestring, sizeof(cfg.check_start) - 1);
        if ((it = cJSON_GetObjectItem(sub, "check_end")) && cJSON_IsString(it))
            strncpy(cfg.check_end, it->valuestring, sizeof(cfg.check_end) - 1);
        if ((it = cJSON_GetObjectItem(sub, "relay_action")) && cJSON_IsString(it))
            strncpy(cfg.relay_action, it->valuestring, sizeof(cfg.relay_action) - 1);
        config_save_timer(&cfg);
    }

    sub = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(sub)) {
        ls_wifi_config_t cfg;
        config_load_wifi(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "primary_ssid")) && cJSON_IsString(it))
            strncpy(cfg.primary_ssid, it->valuestring, MAX_SSID_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "primary_password")) && cJSON_IsString(it))
            strncpy(cfg.primary_password, it->valuestring, MAX_PASSWORD_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "secondary_ssid")) && cJSON_IsString(it))
            strncpy(cfg.secondary_ssid, it->valuestring, MAX_SSID_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "secondary_password")) && cJSON_IsString(it))
            strncpy(cfg.secondary_password, it->valuestring, MAX_PASSWORD_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "ap_mode_enabled")))
            cfg.ap_mode_enabled = cJSON_IsTrue(it);
        config_save_wifi(&cfg);
    }

    sub = cJSON_GetObjectItem(root, "mail");
    if (cJSON_IsObject(sub)) {
        mail_config_t cfg;
        config_load_mail(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "server")) && cJSON_IsString(it))
            strncpy(cfg.server, it->valuestring, sizeof(cfg.server) - 1);
        if ((it = cJSON_GetObjectItem(sub, "port")))
            cfg.port = it->valueint;
        if ((it = cJSON_GetObjectItem(sub, "username")) && cJSON_IsString(it))
            strncpy(cfg.username, it->valuestring, sizeof(cfg.username) - 1);
        if ((it = cJSON_GetObjectItem(sub, "password")) && cJSON_IsString(it))
            strncpy(cfg.password, it->valuestring, sizeof(cfg.password) - 1);
        if ((it = cJSON_GetObjectItem(sub, "sender_name")) && cJSON_IsString(it))
            strncpy(cfg.sender_name, it->valuestring, sizeof(cfg.sender_name) - 1);
        config_save_mail(&cfg);
    }

    cJSON *groups_arr = cJSON_GetObjectItem(root, "mail_groups");
    if (cJSON_IsArray(groups_arr)) {
        int count = cJSON_GetArraySize(groups_arr);
        for (int i = 0; i < count && i < MAX_MAIL_GROUPS; i++) {
            cJSON *g = cJSON_GetArrayItem(groups_arr, i);
            if (!cJSON_IsObject(g)) continue;

            mail_group_t grp = MAIL_GROUP_DEFAULT();
            if ((it = cJSON_GetObjectItem(g, "name")) && cJSON_IsString(it))
                strncpy(grp.name, it->valuestring, MAX_GROUP_NAME_LEN - 1);
            if ((it = cJSON_GetObjectItem(g, "enabled")))
                grp.enabled = cJSON_IsTrue(it);

            cJSON *recips = cJSON_GetObjectItem(g, "recipients");
            if (cJSON_IsArray(recips)) {
                int rc = cJSON_GetArraySize(recips);
                for (int r = 0; r < rc && r < MAX_RECIPIENTS; r++) {
                    cJSON *email = cJSON_GetArrayItem(recips, r);
                    if (cJSON_IsString(email)) {
                        strncpy(grp.recipients[r], email->valuestring, MAX_EMAIL_LEN - 1);
                        grp.recipient_count++;
                    }
                }
            }
            config_save_mail_group(i, &grp);
        }
    }

    sub = cJSON_GetObjectItem(root, "relay");
    if (cJSON_IsObject(sub)) {
        ls_relay_config_t cfg;
        config_load_relay(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "inverted"))) cfg.inverted = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "delay_seconds"))) cfg.delay_seconds = (uint32_t)it->valuedouble;
        if ((it = cJSON_GetObjectItem(sub, "duration_seconds"))) cfg.duration_seconds = (uint32_t)it->valuedouble;
        if ((it = cJSON_GetObjectItem(sub, "pulse_enabled"))) cfg.pulse_enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "pulse_on_ms"))) cfg.pulse_on_ms = (uint32_t)it->valuedouble;
        if ((it = cJSON_GetObjectItem(sub, "pulse_off_ms"))) cfg.pulse_off_ms = (uint32_t)it->valuedouble;
        config_save_relay(&cfg);
    }

    sub = cJSON_GetObjectItem(root, "api");
    if (cJSON_IsObject(sub)) {
        api_config_t cfg;
        config_load_api(&cfg);
        if ((it = cJSON_GetObjectItem(sub, "enabled"))) cfg.enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "endpoint")) && cJSON_IsString(it))
            strncpy(cfg.endpoint, it->valuestring, MAX_HOSTNAME_LEN - 1);
        if ((it = cJSON_GetObjectItem(sub, "require_token"))) cfg.require_token = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(sub, "token")) && cJSON_IsString(it))
            strncpy(cfg.token, it->valuestring, MAX_TOKEN_LEN - 1);
        config_save_api(&cfg);
    }

    cJSON_Delete(root);
    return web_server_send_json(req, "{\"success\":true}");
}
