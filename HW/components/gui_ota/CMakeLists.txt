idf_component_register(
    SRCS "gui_ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ext_flash esp_http_client mbedtls freertos nvs_flash device_log
)
