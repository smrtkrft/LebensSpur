idf_component_register(SRCS "delivery.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES core channels cfg evlog jobs json freertos)
ls_strict_warnings(${COMPONENT_LIB})
