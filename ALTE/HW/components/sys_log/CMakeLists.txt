idf_component_register(
    SRCS "sys_log.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ext_flash esp_timer esp_system heap log freertos
)
