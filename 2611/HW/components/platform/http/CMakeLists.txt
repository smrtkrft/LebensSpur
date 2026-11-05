# http — HTTP sunucusu ve gomulu web arayuzu.
# GUI derlemesi: 2611/GUI/build.py (esbuild + i18n + sprite + gzip + butce).
# Kaynak degisince yeniden uretilir (add_custom_command + GLOB CONFIGURE_DEPENDS);
# ciktilar derleme dizininde, sabit adlarla gomulur.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(INCLUDE_DIRS "include" PRIV_REQUIRES cfg)
    return()
endif()
idf_build_get_property(_python PYTHON)
idf_build_get_property(_build_dir BUILD_DIR)
set(_schema "${_build_dir}/gen/cfg/schema.gen.json")   # cfg bileseninin configure ciktisi
idf_build_get_property(_ver PROJECT_VER)
get_filename_component(GUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../GUI" ABSOLUTE)
set(GUI_OUT "${_build_dir}/gui")
set(GUI_FILES index.html.gz app.js.gz app.css.gz schema.json.gz favicon.svg.gz
    lang/keys.json.gz lang/tr.json.gz lang/en.json.gz lang/de.json.gz
    lang/es.json.gz lang/fr.json.gz lang/it.json.gz)
set(_outs "")
foreach(f ${GUI_FILES} gui_hash.txt)
    list(APPEND _outs "${GUI_OUT}/${f}")
endforeach()
file(GLOB_RECURSE _gui_src CONFIGURE_DEPENDS "${GUI_DIR}/src/*")

idf_component_register(SRCS "http.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES cfg)
ls_strict_warnings(${COMPONENT_LIB})

add_custom_command(
    OUTPUT ${_outs}
    COMMAND ${_python} "${GUI_DIR}/build.py" --src "${GUI_DIR}/src" --out "${GUI_OUT}"
            --ver "${_ver}" --schema "${_schema}"
    DEPENDS ${_gui_src} "${GUI_DIR}/build.py" "${GUI_DIR}/budget.json" "${_schema}"
    COMMENT "Web arayuzu derleniyor (build.py)"
    VERBATIM)
add_custom_target(ls_gui DEPENDS ${_outs})
add_dependencies(${COMPONENT_LIB} ls_gui)
foreach(f ${GUI_FILES})
    target_add_binary_data(${COMPONENT_LIB} "${GUI_OUT}/${f}" BINARY)
endforeach()
target_add_binary_data(${COMPONENT_LIB} "${GUI_OUT}/gui_hash.txt" TEXT)
