idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_system esp_app_format heap)
ls_strict_warnings(${COMPONENT_LIB})
