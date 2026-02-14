#include "api_auth.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "session_auth.h"
#include "esp_log.h"
#include <cJSON.h>
#include <string.h>

static const char *TAG = "AUTH_API";

esp_err_t h_api_login(httpd_req_t *req)
{
    ws_request_count++;
    char body[256] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        ESP_LOGW(TAG, "Login: body okunamadi");
        return web_server_send_error(req, 400, "Bad request");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        ESP_LOGW(TAG, "Login: JSON parse hatasi");
        return web_server_send_error(req, 400, "Invalid JSON");
    }

    cJSON *pw = cJSON_GetObjectItem(json, "password");
    const char *password = cJSON_IsString(pw) ? pw->valuestring : "";

    bool ok = session_check_password(password);
    int pw_len = (int)strlen(password);   // cJSON_Delete'den ONCE kaydet
    cJSON_Delete(json);
    // NOT: password pointer'i artik gecersiz (freed memory)

    if (!ok) {
        ESP_LOGW(TAG, "Login: Sifre yanlis (len=%d)", pw_len);
        httpd_resp_set_status(req, "401 Unauthorized");
        return web_server_send_json(req, "{\"success\":false,\"error\":\"Wrong password\"}");
    }

    char token[SESSION_TOKEN_LEN + 1];
    if (session_create(token) != ESP_OK) {
        ESP_LOGE(TAG, "Login: Session olusturulamadi");
        return web_server_send_error(req, 500, "Session error");
    }

    char cookie[128];
    session_format_cookie(token, cookie, sizeof(cookie));
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"success\":true,\"token\":\"%s\"}", token);
    ESP_LOGI(TAG, "Login: Basarili, token=%.8s..., aktif=%d", token, session_get_active_count());
    return web_server_send_json(req, resp);
}

esp_err_t h_api_logout(httpd_req_t *req)
{
    ws_request_count++;

    char token[SESSION_TOKEN_LEN + 1] = {0};
    char auth_hdr[128] = {0};
    char cookie_hdr[512] = {0};

    httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr));
    httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr));

    if (session_extract_token(auth_hdr[0] ? auth_hdr : NULL,
                              cookie_hdr[0] ? cookie_hdr : NULL, token)) {
        session_destroy(token);
    }

    char logout_cookie[128];
    session_format_logout_cookie(logout_cookie, sizeof(logout_cookie));
    httpd_resp_set_hdr(req, "Set-Cookie", logout_cookie);

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
