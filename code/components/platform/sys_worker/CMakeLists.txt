idf_component_register(SRCS "sys_worker.c" INCLUDE_DIRS "include" PRIV_REQUIRES freertos esp_system)
ls_strict_warnings(${COMPONENT_LIB})
