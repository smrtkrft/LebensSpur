# GUI klasoru bu bilesene gore cozulur (proje dizininden bagimsiz; esp_bin sarmalayicisi ile uyumlu)
get_filename_component(GUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../GUI" ABSOLUTE)

# ---------------------------------------------------------------
# GUI metin dosyalari derleme aninda gzip'lenir:
#   - firmware'de ~420 KB yerine ~95 KB yer kaplar (dahili flash)
#   - WiFi TX suresi ~4x kisalir (guc pikleri)
# Cikti: <build>/esp-idf/ext_flash/gui_gz/*.gz  ->  EMBED_FILES
# Kaynak degisince yeniden configure + gzip (CMAKE_CONFIGURE_DEPENDS).
# config.json duz kalir (surum kontrolu onu okur), PNG'ler zaten sikistirilmis.
# ---------------------------------------------------------------
set(GUI_GZ_DIR "${CMAKE_CURRENT_BINARY_DIR}/gui_gz")
set(GUI_GZ_SOURCES index.html style.css app.js
    lang/en.json lang/de.json lang/es.json lang/fr.json lang/it.json lang/tr.json)
foreach(_f ${GUI_GZ_SOURCES})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GUI_DIR}/${_f}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gzip_gui.py")

idf_build_get_property(_python PYTHON)
if(NOT _python)
    set(_python python3)
endif()
execute_process(
    COMMAND ${_python} "${CMAKE_CURRENT_LIST_DIR}/gzip_gui.py" "${GUI_DIR}" "${GUI_GZ_DIR}" ${GUI_GZ_SOURCES}
    RESULT_VARIABLE _gz_res
    OUTPUT_VARIABLE _gz_out
    ERROR_VARIABLE  _gz_err
)
if(NOT _gz_res EQUAL 0)
    message(FATAL_ERROR "GUI gzip basarisiz: ${_gz_err}")
endif()
if(_gz_out)
    message(STATUS "${_gz_out}")
endif()

idf_component_register(
    SRCS "ext_flash.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver spi_flash littlefs vfs nvs_flash
    EMBED_TXTFILES
        "${GUI_DIR}/config.json"
    EMBED_FILES
        "${GUI_GZ_DIR}/index.html.gz"
        "${GUI_GZ_DIR}/style.css.gz"
        "${GUI_GZ_DIR}/app.js.gz"
        "${GUI_GZ_DIR}/lang/en.json.gz"
        "${GUI_GZ_DIR}/lang/de.json.gz"
        "${GUI_GZ_DIR}/lang/es.json.gz"
        "${GUI_GZ_DIR}/lang/fr.json.gz"
        "${GUI_GZ_DIR}/lang/it.json.gz"
        "${GUI_GZ_DIR}/lang/tr.json.gz"
        "${GUI_DIR}/pic/logo.png"
        "${GUI_DIR}/pic/darklogo.png"
)
