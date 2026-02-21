idf_component_register(
    SRCS "telegram_client.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client mbedtls nvs_flash log
)
