/* http iç yapıları (http.c, http_auth.c, http_sse.c) */
#pragma once
#include "cJSON.h"
#include "http.h"

#define HTTP_BODY_MAX   2048
#define HTTP_RESP_MAX   4096
#define HTTP_MAX_PARAMS 3
#define HTTP_MAX_HDRS   4

struct http_req {
    httpd_req_t *req;
    cJSON *body;
    const char *params[HTTP_MAX_PARAMS];
    char param_buf[HTTP_MAX_PARAMS][64];
    char ip[16];
    char hdr_name[HTTP_MAX_HDRS][24];
    char hdr_val[HTTP_MAX_HDRS][160];    /* httpd değer işaretçisini saklar: istek boyunca yaşamalı (W1) */
    int nhdr;
};

void http_auth_init(void);
bool http_auth_token_alive(const char *tok);
void http_auth_cookie(http_req_t *r, char out[33]);
void http_sse_init(httpd_handle_t h);
httpd_handle_t http_server(void);
