idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES wifi_manager web_server ext_flash flash_sync timer_manager smtp_client telegram_client device_log output nvs_flash spi_flash esp_partition gui_ota fw_ota app_update
)
