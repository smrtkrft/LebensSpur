idf_component_register(SRCS "api.c" "api_groups.c" INCLUDE_DIRS "include"
                       REQUIRES http
                       PRIV_REQUIRES cfg core evlog jobs net_wifi storage esp_app_format heap json)
ls_strict_warnings(${COMPONENT_LIB})
