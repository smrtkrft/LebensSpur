cmake_minimum_required(VERSION 3.16)

# =====================================================================
# LebensSpur 2611 — build sarmalayici projesi
#
# Kaynaklar 2611/HW altinda durur ve oraya hicbir sey yazilmaz.
# Uretilen dosyalar (build/, sdkconfig, managed_components/) bu klasorde.
#   ./esp_bin/build.sh                 (build)
#   ./esp_bin/build.sh -p PORT flash
# =====================================================================

get_filename_component(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../2611/HW" ABSOLUTE)

set(SDKCONFIG_DEFAULTS
    "${SRC_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.esp_bin")

# PROJECT_VER tek kaynak: 2611/HW/CMakeLists.txt
file(STRINGS "${SRC_DIR}/CMakeLists.txt" _ver_line REGEX "set\\(PROJECT_VER")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" PROJECT_VER "${_ver_line}")
message(STATUS "LebensSpur 2611 PROJECT_VER = ${PROJECT_VER}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRC_DIR}/CMakeLists.txt")

set(EXTRA_COMPONENT_DIRS
    "${SRC_DIR}/main"
    "${SRC_DIR}/components/platform"
    "${SRC_DIR}/components/app")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Proje geneli ayarlar (uyari seti, cJSON ic ice siniri): idf_build_* project.cmake sonrasi tanimli
include("${SRC_DIR}/cmake/project_settings.cmake")
project(lebensspur)
