#ifndef API_MAIL_H
#define API_MAIL_H

#include "esp_http_server.h"

esp_err_t h_api_config_mail_get(httpd_req_t *req);
esp_err_t h_api_config_mail_post(httpd_req_t *req);
esp_err_t h_api_mail_test(httpd_req_t *req);
esp_err_t h_api_mail_stats(httpd_req_t *req);
esp_err_t h_api_config_smtp_get(httpd_req_t *req);
esp_err_t h_api_config_smtp_post(httpd_req_t *req);
esp_err_t h_api_test_smtp(httpd_req_t *req);
esp_err_t h_api_config_mail_groups_get(httpd_req_t *req);
esp_err_t h_api_config_mail_groups_post(httpd_req_t *req);

#endif
