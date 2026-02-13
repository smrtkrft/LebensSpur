#include "api_auth.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "session_auth.h"
#include <cJSON.h>
#include <string.h>

esp_err_t h_api_login(httpd_req_t *req)
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

    bool ok = session_check_password(password);
    cJSON_Delete(json);

    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return web_server_send_json(req, "{\"success\":false,\"error\":\"Wrong password\"}");
    }

    char token[SESSION_TOKEN_LEN + 1];
    if (session_create(token) != ESP_OK) {
        return web_server_send_error(req, 500, "Session error");
    }

    char cookie[128];
    session_format_cookie(token, cookie, sizeof(cookie));
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"success\":true,\"token\":\"%s\"}", token);
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
