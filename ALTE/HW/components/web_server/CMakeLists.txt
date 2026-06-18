idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server ext_flash wifi_manager timer_manager smtp_client telegram_client system_info device_log output nvs_flash esp_event esp_netif esp_system gui_ota fw_ota
)
