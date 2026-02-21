#include "api_mail.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "config_manager.h"
#include "mail_sender.h"
#include <cJSON.h>
#include <string.h>

esp_err_t h_api_config_mail_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    mail_config_t cfg;
    if (config_load_mail(&cfg) != ESP_OK) {
        mail_config_t def = MAIL_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    char json[512];
    snprintf(json, sizeof(json),
        "{\"server\":\"%s\","
        "\"port\":%d,"
        "\"username\":\"%s\","
        "\"password\":\"%s\","
        "\"sender_name\":\"%s\"}",
        cfg.server, cfg.port, cfg.username,
        cfg.password[0] ? "********" : "",
        cfg.sender_name);

    return web_server_send_json(req, json);
}

esp_err_t h_api_config_mail_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    mail_config_t cfg;
    config_load_mail(&cfg);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "server")) && cJSON_IsString(it))
        strncpy(cfg.server, it->valuestring, sizeof(cfg.server) - 1);
    if ((it = cJSON_GetObjectItem(json, "port")))
        cfg.port = it->valueint;
    if ((it = cJSON_GetObjectItem(json, "username")) && cJSON_IsString(it))
        strncpy(cfg.username, it->valuestring, sizeof(cfg.username) - 1);
    if ((it = cJSON_GetObjectItem(json, "password")) && cJSON_IsString(it)) {
        if (strcmp(it->valuestring, "********") != 0 && strlen(it->valuestring) > 0)
            strncpy(cfg.password, it->valuestring, sizeof(cfg.password) - 1);
    }
    if ((it = cJSON_GetObjectItem(json, "sender_name")) && cJSON_IsString(it))
        strncpy(cfg.sender_name, it->valuestring, sizeof(cfg.sender_name) - 1);

    cJSON_Delete(json);

    if (config_save_mail(&cfg) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

esp_err_t h_api_mail_test(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *to_item = cJSON_GetObjectItem(json, "to");
    const char *to = cJSON_IsString(to_item) ? to_item->valuestring : NULL;

    if (!to || strlen(to) == 0) {
        cJSON_Delete(json);
        return web_server_send_error(req, 400, "Missing 'to'");
    }

    esp_err_t ret = mail_send_test(to);
    cJSON_Delete(json);

    if (ret == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true,\"message\":\"Test mail queued\"}");
    }
    return web_server_send_error(req, 500, "Mail queue failed");
}

esp_err_t h_api_mail_stats(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    mail_stats_t stats;
    mail_get_stats(&stats);

    char json[256];
    snprintf(json, sizeof(json),
        "{\"total_sent\":%lu,\"total_failed\":%lu,\"queue_count\":%lu,\"last_send_time\":%lu}",
        stats.total_sent, stats.total_failed, stats.queue_count, stats.last_send_time);

    return web_server_send_json(req, json);
}

esp_err_t h_api_config_smtp_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    mail_config_t cfg;
    if (config_load_mail(&cfg) != ESP_OK) {
        mail_config_t def = MAIL_CONFIG_DEFAULT();
        memcpy(&cfg, &def, sizeof(cfg));
    }

    char json[512];
    snprintf(json, sizeof(json),
        "{\"smtpServer\":\"%s\","
        "\"smtpPort\":%d,"
        "\"smtpUsername\":\"%s\","
        "\"smtpPassword\":\"%s\"}",
        cfg.server, cfg.port, cfg.username,
        cfg.password[0] ? "********" : "");

    return web_server_send_json(req, json);
}

esp_err_t h_api_config_smtp_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    mail_config_t cfg;
    config_load_mail(&cfg);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "smtpServer")) && cJSON_IsString(it))
        strncpy(cfg.server, it->valuestring, sizeof(cfg.server) - 1);
    if ((it = cJSON_GetObjectItem(json, "smtpPort")))
        cfg.port = it->valueint;
    if ((it = cJSON_GetObjectItem(json, "smtpUsername")) && cJSON_IsString(it))
        strncpy(cfg.username, it->valuestring, sizeof(cfg.username) - 1);
    if ((it = cJSON_GetObjectItem(json, "smtpPassword")) && cJSON_IsString(it)) {
        if (strcmp(it->valuestring, "********") != 0 && strlen(it->valuestring) > 0)
            strncpy(cfg.password, it->valuestring, sizeof(cfg.password) - 1);
    }

    cJSON_Delete(json);

    if (config_save_mail(&cfg) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}

esp_err_t h_api_test_smtp(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    mail_result_t result;
    esp_err_t ret = mail_test_connection(&result);

    char json[256];
    if (ret == ESP_OK && result.success) {
        snprintf(json, sizeof(json),
            "{\"success\":true,\"smtp_code\":%d,\"message\":\"Connection OK\"}",
            result.smtp_code);
    } else {
        snprintf(json, sizeof(json),
            "{\"success\":false,\"smtp_code\":%d,\"error\":\"%s\"}",
            result.smtp_code, result.error_msg);
    }

    return web_server_send_json(req, json);
}

esp_err_t h_api_config_mail_groups_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        if (root) cJSON_Delete(root);
        if (arr) cJSON_Delete(arr);
        return web_server_send_error(req, 500, "No memory");
    }

    for (int i = 0; i < MAX_MAIL_GROUPS; i++) {
        mail_group_t grp;
        if (config_load_mail_group(i, &grp) != ESP_OK) {
            mail_group_t def = MAIL_GROUP_DEFAULT();
            memcpy(&grp, &def, sizeof(grp));
        }
        if (grp.name[0] == '\0' && grp.recipient_count == 0) continue;

        cJSON *obj = cJSON_CreateObject();
        if (!obj) continue;

        cJSON_AddStringToObject(obj, "name", grp.name);
        cJSON_AddStringToObject(obj, "subject", "");
        cJSON_AddStringToObject(obj, "content", "");

        cJSON *recips = cJSON_CreateArray();
        if (recips) {
            for (int r = 0; r < grp.recipient_count && r < MAX_RECIPIENTS; r++) {
                cJSON_AddItemToArray(recips, cJSON_CreateString(grp.recipients[r]));
            }
            cJSON_AddItemToObject(obj, "recipients", recips);
        }

        cJSON_AddItemToArray(arr, obj);
    }

    cJSON_AddItemToObject(root, "groups", arr);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

esp_err_t h_api_config_mail_groups_post(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[1024] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *idx_j = cJSON_GetObjectItem(json, "index");
    if (!cJSON_IsNumber(idx_j) || idx_j->valueint < 0 || idx_j->valueint >= MAX_MAIL_GROUPS) {
        cJSON_Delete(json);
        return web_server_send_error(req, 400, "Invalid index");
    }
    int idx = idx_j->valueint;

    mail_group_t grp;
    config_load_mail_group(idx, &grp);

    cJSON *it;
    if ((it = cJSON_GetObjectItem(json, "name")) && cJSON_IsString(it))
        strncpy(grp.name, it->valuestring, MAX_GROUP_NAME_LEN - 1);
    if ((it = cJSON_GetObjectItem(json, "enabled")))
        grp.enabled = cJSON_IsTrue(it);

    cJSON *recips = cJSON_GetObjectItem(json, "recipients");
    if (cJSON_IsArray(recips)) {
        grp.recipient_count = 0;
        int count = cJSON_GetArraySize(recips);
        for (int r = 0; r < count && r < MAX_RECIPIENTS; r++) {
            cJSON *email = cJSON_GetArrayItem(recips, r);
            if (cJSON_IsString(email)) {
                strncpy(grp.recipients[r], email->valuestring, MAX_EMAIL_LEN - 1);
                grp.recipient_count++;
            }
        }
    }

    cJSON_Delete(json);

    if (config_save_mail_group(idx, &grp) == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Save failed");
}
