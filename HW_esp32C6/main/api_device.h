#ifndef API_DEVICE_H
#define API_DEVICE_H

#include "esp_http_server.h"

esp_err_t h_api_device_info(httpd_req_t *req);
esp_err_t h_api_status(httpd_req_t *req);
esp_err_t h_api_reboot(httpd_req_t *req);
esp_err_t h_api_factory_reset(httpd_req_t *req);

#endif
