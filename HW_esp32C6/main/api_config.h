#ifndef API_CONFIG_H
#define API_CONFIG_H

#include "esp_http_server.h"

esp_err_t h_api_config_export(httpd_req_t *req);
esp_err_t h_api_config_import(httpd_req_t *req);
esp_err_t h_api_config_security_get(httpd_req_t *req);
esp_err_t h_api_config_security_post(httpd_req_t *req);
esp_err_t h_api_config_security_apikey(httpd_req_t *req);
esp_err_t h_api_config_webhook(httpd_req_t *req);
esp_err_t h_api_config_telegram(httpd_req_t *req);
esp_err_t h_api_config_early_mail(httpd_req_t *req);

#endif
