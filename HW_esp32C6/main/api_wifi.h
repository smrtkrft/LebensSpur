#ifndef API_WIFI_H
#define API_WIFI_H

#include "esp_http_server.h"

esp_err_t h_api_wifi_status(httpd_req_t *req);
esp_err_t h_api_config_wifi_get(httpd_req_t *req);
esp_err_t h_api_config_wifi_post(httpd_req_t *req);
esp_err_t h_api_config_ap(httpd_req_t *req);

#endif
