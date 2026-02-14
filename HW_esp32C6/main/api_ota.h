#ifndef API_OTA_H
#define API_OTA_H

#include "esp_http_server.h"

// Firmware OTA
esp_err_t h_api_ota_status(httpd_req_t *req);
esp_err_t h_api_ota_url(httpd_req_t *req);
esp_err_t h_api_ota_check(httpd_req_t *req);

// GUI Slot
esp_err_t h_api_gui_health(httpd_req_t *req);
esp_err_t h_api_gui_slot_status(httpd_req_t *req);
esp_err_t h_api_gui_rollback(httpd_req_t *req);
esp_err_t h_api_gui_download(httpd_req_t *req);
esp_err_t h_api_gui_download_status(httpd_req_t *req);

#endif
