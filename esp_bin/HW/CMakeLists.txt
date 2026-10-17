cmake_minimum_required(VERSION 3.16)

# =====================================================================
# LebensSpur HW — build sarmalayici projesi
#
# Kaynaklar ALTE/HW altinda durur ve oraya hicbir sey yazilmaz.
# Tum uretilen dosyalar (build/, sdkconfig, managed_components/)
# bu klasorde (esp_bin/HW) olusur.
#
# Kullanim:  ./esp_bin/build.sh            (build)
#            ./esp_bin/build.sh flash monitor
# =====================================================================

get_filename_component(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../ALTE/HW" ABSOLUTE)

# sdkconfig.defaults kaynaktan; partition tablosu yolu icin yerel ek dosya
set(SDKCONFIG_DEFAULTS
    "${SRC_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.esp_bin")

# PROJECT_VER'i ALTE/HW/CMakeLists.txt icinden oku (tek kaynak)
file(STRINGS "${SRC_DIR}/CMakeLists.txt" _ver_line REGEX "set\\(PROJECT_VER")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" PROJECT_VER "${_ver_line}")
message(STATUS "LebensSpur PROJECT_VER = ${PROJECT_VER} (kaynak: ALTE/HW/CMakeLists.txt)")
# Kaynak CMakeLists degisince (surum artisi) yeniden yapilandir; yoksa eski surum gomulu kalir
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRC_DIR}/CMakeLists.txt")

# main ve components kaynaktan
set(EXTRA_COMPONENT_DIRS "${SRC_DIR}/main" "${SRC_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lebensspur)
