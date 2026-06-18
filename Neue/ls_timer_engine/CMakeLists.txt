idf_component_register(
    SRCS "src/ls_timer_engine.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES sk_core nvs_flash esp_timer
)
