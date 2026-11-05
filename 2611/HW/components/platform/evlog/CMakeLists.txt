idf_component_register(SRCS "evlog_classify.c" "evlog.c" "jsonl_tail.c" "evlog_file.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer heap storage sys_worker)
ls_strict_warnings(${COMPONENT_LIB})
