idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_system esp_app_format heap http cfg evlog storage esp_timer sys_worker core io)
ls_strict_warnings(${COMPONENT_LIB})
