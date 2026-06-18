idf_component_register(
    SRCS "src/sk_api.c"
    INCLUDE_DIRS "include"
    REQUIRES sk_core esp_http_client
    PRIV_REQUIRES nvs_flash esp_timer esp-tls mbedtls
)
