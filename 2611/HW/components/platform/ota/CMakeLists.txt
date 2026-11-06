idf_component_register(SRCS "ota.c" "semver.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_https_ota esp_http_client app_update esp_app_format esp_timer mbedtls
                                     evlog jobs net_wifi sys_worker log)
ls_strict_warnings(${COMPONENT_LIB})
