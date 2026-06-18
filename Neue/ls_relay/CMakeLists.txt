idf_component_register(
    SRCS "src/ls_relay.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES sk_core nvs_flash driver esp_timer
)
