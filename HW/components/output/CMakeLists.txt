idf_component_register(
    SRCS "output.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_timer nvs_flash log esp_system
)
