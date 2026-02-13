#ifndef API_AUTH_H
#define API_AUTH_H

#include "esp_http_server.h"

esp_err_t h_api_login(httpd_req_t *req);
esp_err_t h_api_logout(httpd_req_t *req);

#endif
