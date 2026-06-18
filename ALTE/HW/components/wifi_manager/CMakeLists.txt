idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash mdns freertos device_log
)
