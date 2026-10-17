idf_component_register(
    SRCS "api_client.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client mqtt mbedtls nvs_flash log json ls_util sys_log wifi_manager esp_timer
)
