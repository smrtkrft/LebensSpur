idf_component_register(SRCS "io.c" INCLUDE_DIRS "include" PRIV_REQUIRES esp_driver_gpio esp_timer esp_system hal soc)
ls_strict_warnings(${COMPONENT_LIB})
