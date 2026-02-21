#ifndef API_SETUP_H
#define API_SETUP_H

#include "esp_http_server.h"

esp_err_t h_api_setup_status(httpd_req_t *req);
esp_err_t h_api_setup_wifi_scan(httpd_req_t *req);
esp_err_t h_api_setup_wifi_connect(httpd_req_t *req);
esp_err_t h_api_setup_password(httpd_req_t *req);
esp_err_t h_api_setup_complete(httpd_req_t *req);
esp_err_t h_api_password_change(httpd_req_t *req);
esp_err_t h_api_gui_download(httpd_req_t *req);
esp_err_t h_api_gui_download_status(httpd_req_t *req);

#endif
