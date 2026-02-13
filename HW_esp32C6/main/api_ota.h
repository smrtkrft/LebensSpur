#ifndef API_OTA_H
#define API_OTA_H

#include "esp_http_server.h"

esp_err_t h_api_ota_status(httpd_req_t *req);
esp_err_t h_api_ota_url(httpd_req_t *req);
esp_err_t h_api_ota_check(httpd_req_t *req);

#endif
