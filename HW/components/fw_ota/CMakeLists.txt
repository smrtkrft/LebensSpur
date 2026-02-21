idf_component_register(
    SRCS "fw_ota.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_https_ota esp_http_client mbedtls app_update freertos esp_partition
)
