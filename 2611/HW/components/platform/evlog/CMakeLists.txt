idf_component_register(SRCS "evlog_classify.c" "evlog.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer heap)
ls_strict_warnings(${COMPONENT_LIB})
