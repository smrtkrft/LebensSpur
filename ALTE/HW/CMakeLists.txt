cmake_minimum_required(VERSION 3.16)

# Build: ./esp_bin/build.sh (sarmalayici proje esp_bin/HW); bu dosya PROJECT_VER icin tek kaynaktir
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

# Firmware versiyonu (esp_app_desc_t icin - OTA versiyon kontrolunde kullanilir)
set(PROJECT_VER "0.0.70")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lebensspur)
