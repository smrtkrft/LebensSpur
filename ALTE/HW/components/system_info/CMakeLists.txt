idf_component_register(
    SRCS "system_info.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_hw_support esp_wifi esp_timer spi_flash heap log esp_partition
)
