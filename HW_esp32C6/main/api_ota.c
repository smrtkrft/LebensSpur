#include "api_ota.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "ota_manager.h"
#include "gui_slot.h"
#include "gui_downloader.h"
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

// ============================================================================
// GUI Slot API
// ============================================================================

esp_err_t h_api_gui_health(httpd_req_t *req)
{
    // Health ping — auth gereksiz (GUI yuklendigini dogrulamak icin)
    ws_request_count++;
    gui_slot_health_ok();
    return web_server_send_json(req, "{\"success\":true}");
}

esp_err_t h_api_gui_slot_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    const gui_slot_meta_t *m = gui_slot_get_meta();

    char json[256];
    snprintf(json, sizeof(json),
        "{\"active\":\"%s\","
        "\"ver_active\":\"%s\","
        "\"ver_backup\":\"%s\","
        "\"slot_a\":\"%s\","
        "\"slot_b\":\"%s\","
        "\"boot_count\":%d,"
        "\"has_gui\":%s}",
        m->active == GUI_SLOT_B ? "b" : "a",
        gui_slot_get_active_version(),
        gui_slot_get_backup_version(),
        m->ver_a,
        m->ver_b,
        m->boot_count,
        gui_slot_has_gui() ? "true" : "false");

    return web_server_send_json(req, json);
}

esp_err_t h_api_gui_rollback(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    esp_err_t ret = gui_slot_rollback();
    if (ret == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true,\"message\":\"Rollback OK. Sayfayi yenileyin.\"}");
    }
    return web_server_send_error(req, 400, "Yedek slot bos, rollback yapilamaz");
}

esp_err_t h_api_gui_download(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    // Inaktif slot'a indir
    esp_err_t ret = gui_downloader_start(NULL, NULL, NULL);
    if (ret == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Download baslatilamadi");
}

esp_err_t h_api_gui_download_status(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    gui_dl_status_t st;
    gui_downloader_get_status(&st);

    char json[512];
    snprintf(json, sizeof(json),
        "{\"state\":%d,\"progress\":%d,\"message\":\"%s\","
        "\"error\":\"%s\",\"bytes\":%lu,\"files\":%d,\"total_files\":%d}",
        (int)st.state, st.progress, st.message,
        st.error, (unsigned long)st.bytes_downloaded,
        st.files_downloaded, st.total_files);

    return web_server_send_json(req, json);
}
