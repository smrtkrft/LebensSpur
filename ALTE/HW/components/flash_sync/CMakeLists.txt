idf_component_register(
    SRCS "flash_sync.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ext_flash nvs_flash esp_partition spi_flash
)
