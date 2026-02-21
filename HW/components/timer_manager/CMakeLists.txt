idf_component_register(
    SRCS "timer_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_timer
)
