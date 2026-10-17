idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES wifi_manager web_server ext_flash timer_manager smtp_client telegram_client api_client sys_log output nvs_flash spi_flash esp_partition gui_ota fw_ota app_update esp_timer
)
