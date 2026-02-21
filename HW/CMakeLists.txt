cmake_minimum_required(VERSION 3.16)

# Build dizinini TEST2/Build olarak ayarla
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

# Firmware versiyonu (esp_app_desc_t icin - OTA versiyon kontrolunde kullanilir)
set(PROJECT_VER "0.0.67")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lebensspur)
