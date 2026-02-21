#include "api_setup.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "config_manager.h"
#include "session_auth.h"
#include "wifi_manager.h"
#include "gui_downloader.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

esp_err_t h_api_setup_status(httpd_req_t *req)
{
    ws_request_count++;
    char json[64];
    snprintf(json, sizeof(json), "{\"setup_completed\":%s}",
             config_is_setup_completed() ? "true" : "false");
    return web_server_send_json(req, json);
}

esp_err_t h_api_setup_wifi_scan(httpd_req_t *req)
{
    ws_request_count++;

    if (wifi_manager_scan() != ESP_OK) {
        return web_server_send_error(req, 500, "Scan failed");
    }

    wifi_ap_record_t recs[WIFI_MAX_SCAN_RESULTS];
    uint16_t count = WIFI_MAX_SCAN_RESULTS;
    wifi_manager_scan_get_results(recs, &count);

    char *json = malloc(2048);
    if (!json) return web_server_send_error(req, 500, "No memory");

    strcpy(json, "{\"networks\":[");
    for (int i = 0; i < count && i < 15; i++) {
        char entry[128];
        snprintf(entry, sizeof(entry),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d}",
            (i > 0) ? "," : "",
            (char *)recs[i].ssid, recs[i].rssi, recs[i].primary);
        strcat(json, entry);
    }
    strcat(json, "]}");

    esp_err_t ret = web_server_send_json(req, json);
    free(json);
    return ret;
}

esp_err_t h_api_setup_wifi_connect(httpd_req_t *req)
{
    ws_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *ssid_j = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_j = cJSON_GetObjectItem(json, "password");
    const char *ssid = cJSON_IsString(ssid_j) ? ssid_j->valuestring : "";
    const char *pass = cJSON_IsString(pass_j) ? pass_j->valuestring : "";

    ls_wifi_config_t wcfg = LS_WIFI_CONFIG_DEFAULT();
    strncpy(wcfg.primary_ssid, ssid, MAX_SSID_LEN - 1);
    strncpy(wcfg.primary_password, pass, MAX_PASSWORD_LEN - 1);
    config_save_wifi(&wcfg);

    esp_err_t ret = wifi_manager_connect(ssid, pass);
    cJSON_Delete(json);

    if (ret == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Connect failed");
}

esp_err_t h_api_setup_password(httpd_req_t *req)
{
    ws_request_count++;

    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *pw = cJSON_GetObjectItem(json, "password");
    const char *password = cJSON_IsString(pw) ? pw->valuestring : "";

    esp_err_t ret = session_set_initial_password(password);
    cJSON_Delete(json);

    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    if (ret == ESP_ERR_INVALID_SIZE) return web_server_send_error(req, 400, "Password too short (min 1 char)");
    return web_server_send_error(req, 500, "System not ready");
}

esp_err_t h_api_setup_complete(httpd_req_t *req)
{
    ws_request_count++;
    config_mark_setup_completed();
    return web_server_send_json(req, "{\"success\":true}");
}

esp_err_t h_api_password_change(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    char body[512] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) return web_server_send_error(req, 400, "Invalid JSON");

    cJSON *cur = cJSON_GetObjectItem(json, "currentPassword");
    if (!cur) cur = cJSON_GetObjectItem(json, "current_password");
    cJSON *nw = cJSON_GetObjectItem(json, "newPassword");
    if (!nw) nw = cJSON_GetObjectItem(json, "new_password");
    const char *cur_pw = cJSON_IsString(cur) ? cur->valuestring : "";
    const char *new_pw = cJSON_IsString(nw) ? nw->valuestring : "";

    esp_err_t ret = session_change_password(cur_pw, new_pw);
    cJSON_Delete(json);

    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    if (ret == ESP_ERR_INVALID_ARG) return web_server_send_error(req, 400, "Wrong current password");
    if (ret == ESP_ERR_INVALID_SIZE) return web_server_send_error(req, 400, "Password too short");
    return web_server_send_error(req, 500, "Password change failed");
}

esp_err_t h_api_gui_download(httpd_req_t *req)
{
    ws_request_count++;

    char body[256] = {0};
    const char *repo = NULL, *branch = NULL, *path = NULL;

    if (req->content_len > 0 && (size_t)req->content_len < sizeof(body)) {
        httpd_req_recv(req, body, req->content_len);
        cJSON *json = cJSON_Parse(body);
        if (json) {
            cJSON *it;
            if ((it = cJSON_GetObjectItem(json, "repo")) && cJSON_IsString(it)) repo = it->valuestring;
            if ((it = cJSON_GetObjectItem(json, "branch")) && cJSON_IsString(it)) branch = it->valuestring;
            if ((it = cJSON_GetObjectItem(json, "path")) && cJSON_IsString(it)) path = it->valuestring;
            esp_err_t ret = gui_downloader_start(repo, branch, path);
            cJSON_Delete(json);
            if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
            return web_server_send_error(req, 500, "Download start failed");
        }
    }

    esp_err_t ret = gui_downloader_start(NULL, NULL, NULL);
    if (ret == ESP_OK) return web_server_send_json(req, "{\"success\":true}");
    return web_server_send_error(req, 500, "Download start failed");
}

esp_err_t h_api_gui_download_status(httpd_req_t *req)
{
    ws_request_count++;

    gui_dl_status_t st;
    gui_downloader_get_status(&st);

    const char *snames[] = {"idle", "connecting", "downloading", "installing", "complete", "error"};

    char json[512];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"progress\":%d,\"message\":\"%s\",\"error\":\"%s\","
        "\"bytes_downloaded\":%lu,\"files_downloaded\":%d,\"total_files\":%d}",
        snames[st.state], st.progress, st.message, st.error,
        st.bytes_downloaded, st.files_downloaded, st.total_files);

    return web_server_send_json(req, json);
}
