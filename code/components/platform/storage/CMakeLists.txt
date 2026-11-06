idf_component_register(SRCS "storage.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_spi spi_flash esp_partition freertos littlefs)
ls_strict_warnings(${COMPONENT_LIB})
