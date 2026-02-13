#ifndef API_LOGS_H
#define API_LOGS_H

#include "esp_http_server.h"

esp_err_t h_api_logs_get(httpd_req_t *req);
esp_err_t h_api_logs_delete(httpd_req_t *req);

#endif
