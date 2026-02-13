#ifndef API_TIMER_H
#define API_TIMER_H

#include "esp_http_server.h"

esp_err_t h_api_timer_status(httpd_req_t *req);
esp_err_t h_api_timer_reset(httpd_req_t *req);
esp_err_t h_api_config_timer_get(httpd_req_t *req);
esp_err_t h_api_config_timer_post(httpd_req_t *req);
esp_err_t h_api_timer_enable(httpd_req_t *req);
esp_err_t h_api_timer_disable(httpd_req_t *req);
esp_err_t h_api_timer_acknowledge(httpd_req_t *req);
esp_err_t h_api_timer_vacation(httpd_req_t *req);

#endif
