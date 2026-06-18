idf_component_register(
    SRCS "smtp_client.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls nvs_flash log lwip
)
