idf_component_register(
    SRCS "src/ls_reset_api.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES sk_core nvs_flash esp_http_server esp_event
)
