idf_component_register(
    SRCS "src/ls_smtp.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES sk_core nvs_flash esp-tls mbedtls
)
