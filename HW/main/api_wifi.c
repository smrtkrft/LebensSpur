#include "api_wifi.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include <cJSON.h>
#include <string.h>

esp_err_t h_api_wifi_status(httpd_req_t *req)
{
    ws_request_count++;

    char json[256];
    snprintf(json, sizeof(json),
        "{\"connected\":%s,\"sta_ip\":\"%s\",\"ap_ip\":\"%s\",\"ap_ssid\":\"%s\"}",
        wifi_manager_is_connected() ? "true" : "false",
        wifi_manager_get_ip(),
        wifi_manager_get_ap_ip(),
        wifi_manager_get_ap_ssid());

    return web_server_send_json(req, json);
}

esp_err_t h_api_config_wifi_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    ls_wifi_config_t cfg;
    if (config_load_wifi(&cfg) != ESP_OK) {
        ls_wifi_config_t def = LS_WIFI_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return web_server_send_error(req, 500, "No memory");

    cJSON *primary = cJSON_CreateObject();
    cJSON_AddStringToObject(primary, "ssid", cfg.primary_ssid);
    cJSON_AddStringToObject(primary, "password", cfg.primary_password[0] ? "********" : "");
    cJSON_AddBoolToObject(primary, "staticIpEnabled", cfg.primary_static_enabled);
    cJSON_AddStringToObject(primary, "staticIp", cfg.primary_static.ip);
    cJSON_AddStringToObject(primary, "gateway", cfg.primary_static.gateway);
    cJSON_AddStringToObject(primary, "subnet", cfg.primary_static.subnet);
    cJSON_AddStringToObject(primary, "dns", cfg.primary_static.dns);
    cJSON_AddItemToObject(root, "primary", primary);

    cJSON *backup = cJSON_CreateObject();
    cJSON_AddStringToObject(backup, "ssid", cfg.secondary_ssid);
    cJSON_AddStringToObject(backup, "password", cfg.secondary_password[0] ? "********" : "");
    cJSON_AddBoolToObject(backup, "staticIpEnabled", cfg.secondary_static_enabled);
    cJSON_AddStringToObject(backup, "staticIp", cfg.secondary_static.ip);
    cJSON_AddStringToObject(backup, "gateway", cfg.secondary_static.gateway);
    cJSON_AddStringToObject(backup, "subnet", cfg.secondary_static.subnet);
    cJSON_AddStringToObject(backup, "dns", cfg.secondary_static.dns);
    cJSON_AddItemToObject(root, "backup", backup);

    cJSON_AddStringToObject(root, "hostname", cfg.primary_mdns);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

esp_err_t h_api_config_wifi_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[1024] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    ls_wifi_config_t cfg;
    if (config_load_wifi(&cfg) != ESP_OK) {
        ls_wifi_config_t def = LS_WIFI_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    cJSON *target = cJSON_GetObjectItem(json, "type");
    if (!target) target = cJSON_GetObjectItem(json, "target");
    const char *tgt = cJSON_IsString(target) ? target->valuestring : "primary";

    cJSON *it;
    if (strcmp(tgt, "backup") == 0 || strcmp(tgt, "secondary") == 0) {
        if ((it = cJSON_GetObjectItem(json, "ssid")) && cJSON_IsString(it))
            strncpy(cfg.secondary_ssid, it->valuestring, MAX_SSID_LEN - 1);
        if ((it = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(it)) {
            if (strcmp(it->valuestring, "********") != 0)
                strncpy(cfg.secondary_password, it->valuestring, MAX_PASSWORD_LEN - 1);
        }
        if ((it = cJSON_GetObjectItem(json, "staticIpEnabled")))
            cfg.secondary_static_enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(json, "staticIp")) && cJSON_IsString(it))
            strncpy(cfg.secondary_static.ip, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "gateway")) && cJSON_IsString(it))
            strncpy(cfg.secondary_static.gateway, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "subnet")) && cJSON_IsString(it))
            strncpy(cfg.secondary_static.subnet, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "dns")) && cJSON_IsString(it))
            strncpy(cfg.secondary_static.dns, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "mdnsHostname")) && cJSON_IsString(it))
            strncpy(cfg.secondary_mdns, it->valuestring, MAX_HOSTNAME_LEN - 1);
    } else {
        if ((it = cJSON_GetObjectItem(json, "ssid")) && cJSON_IsString(it))
            strncpy(cfg.primary_ssid, it->valuestring, MAX_SSID_LEN - 1);
        if ((it = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(it)) {
            if (strcmp(it->valuestring, "********") != 0)
                strncpy(cfg.primary_password, it->valuestring, MAX_PASSWORD_LEN - 1);
        }
        if ((it = cJSON_GetObjectItem(json, "staticIpEnabled")))
            cfg.primary_static_enabled = cJSON_IsTrue(it);
        if ((it = cJSON_GetObjectItem(json, "staticIp")) && cJSON_IsString(it))
            strncpy(cfg.primary_static.ip, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "gateway")) && cJSON_IsString(it))
            strncpy(cfg.primary_static.gateway, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "subnet")) && cJSON_IsString(it))
            strncpy(cfg.primary_static.subnet, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "dns")) && cJSON_IsString(it))
            strncpy(cfg.primary_static.dns, it->valuestring, 15);
        if ((it = cJSON_GetObjectItem(json, "mdnsHostname")) && cJSON_IsString(it))
            strncpy(cfg.primary_mdns, it->valuestring, MAX_HOSTNAME_LEN - 1);
    }

    if ((it = cJSON_GetObjectItem(json, "ap_mode_enabled")))
        cfg.ap_mode_enabled = cJSON_IsTrue(it);

    cJSON_Delete(json);

    if (config_save_wifi(&cfg) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

esp_err_t h_api_config_ap(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    ls_wifi_config_t cfg;
    if (config_load_wifi(&cfg) != ESP_OK) {
        ls_wifi_config_t def = LS_WIFI_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    cJSON *it = cJSON_GetObjectItem(json, "enabled");
    if (!it) it = cJSON_GetObjectItem(json, "ap_mode_enabled");
    if (it) cfg.ap_mode_enabled = cJSON_IsTrue(it);

    cJSON_Delete(json);

    if (config_save_wifi(&cfg) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}
