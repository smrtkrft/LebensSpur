idf_component_register(SRCS "net_tls.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp-tls mbedtls esp_timer lwip)
ls_strict_warnings(${COMPONENT_LIB})
