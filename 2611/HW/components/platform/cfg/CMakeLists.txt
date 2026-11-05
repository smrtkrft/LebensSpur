# cfg — tek yapilandirma semasi (cfg_schema.json -> uretilen C + GUI semasi).
# Uretilen dosyalar derleme dizinine yazilir; 2611/ altina hicbir sey uretilmez.
idf_build_get_property(_python PYTHON)
idf_build_get_property(_build_dir BUILD_DIR)
set(CFG_GEN_DIR "${_build_dir}/gen/cfg")
set(_schema "${CMAKE_CURRENT_LIST_DIR}/cfg_schema.json")
set(_gen "${CMAKE_CURRENT_LIST_DIR}/../../../tools/gen_cfg.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_schema}" "${_gen}")
execute_process(COMMAND ${_python} "${_gen}" "${_schema}" "${CFG_GEN_DIR}"
                RESULT_VARIABLE _res ERROR_VARIABLE _err)
if(NOT _res EQUAL 0)
    message(FATAL_ERROR "cfg semasi uretilemedi: ${_err}")
endif()
idf_build_set_property(LS_GUI_SCHEMA "${CFG_GEN_DIR}/schema.gen.json")

idf_component_register(SRCS "${CFG_GEN_DIR}/cfg_schema.gen.c"
                       INCLUDE_DIRS "include" "${CFG_GEN_DIR}")
ls_strict_warnings(${COMPONENT_LIB})
