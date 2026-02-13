#include "api_ota.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "ota_manager.h"
#include <cJSON.h>
#include <string.h>

esp_err_t h_api_ota_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    const char *snames[] = {"idle", "downloading", "verifying", "updating", "complete", "error"};

    char json[128];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"progress\":%d,\"version\":\"%s\"}",
        snames[ota_manager_get_state()],
        ota_manager_get_progress(),
        ota_manager_get_current_version());

    return web_server_send_json(req, json);
}

esp_err_t h_api_ota_url(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *url_j = cJSON_GetObjectItem(json, "url");
    const char *url = cJSON_IsString(url_j) ? url_j->valuestring : NULL;

    if (!url) {
        cJSON_Delete(json);
        return web_server_send_error(req, 400, "Missing 'url'");
    }

    esp_err_t ret = ota_manager_start_from_url(url);
    cJSON_Delete(json);

    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    return web_server_send_error(req, 500, "OTA failed");
}

esp_err_t h_api_ota_check(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char json[256];
    snprintf(json, sizeof(json),
        "{\"currentVersion\":\"%s\","
        "\"updateAvailable\":false,"
        "\"version\":\"\"}",
        ota_manager_get_current_version());

    return web_server_send_json(req, json);
}
