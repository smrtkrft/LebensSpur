#ifndef API_RELAY_H
#define API_RELAY_H

#include "esp_http_server.h"

esp_err_t h_api_relay_status(httpd_req_t *req);
esp_err_t h_api_relay_control(httpd_req_t *req);
esp_err_t h_api_relay_test(httpd_req_t *req);
esp_err_t h_api_config_relay_get(httpd_req_t *req);
esp_err_t h_api_config_relay_post(httpd_req_t *req);

#endif
