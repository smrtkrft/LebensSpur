idf_component_register(SRCS "api.c" "api_groups.c" "api_sys.c" INCLUDE_DIRS "include"
                       REQUIRES http
                       PRIV_REQUIRES cfg core evlog jobs net_wifi ota storage sys_worker esp_app_format heap json
                                     app_update bootloader_support spi_flash esp_hw_support espcoredump esp_partition)
ls_strict_warnings(${COMPONENT_LIB})
