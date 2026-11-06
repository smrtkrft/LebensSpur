# LebensSpur firmware — ESP-IDF projesi (ESP32-C6, ESP-IDF 5.5.2).
#   Derleme: ./esp_bin/build.sh  (ya da: idf.py build)
# Üretilen dosyalar (build/, sdkconfig) esp_bin/ altına yazılır; kaynak ağacına bir şey üretilmez.
cmake_minimum_required(VERSION 3.16)

# Sürümün TEK kaynağı (arayüz ve OTA da buradan alır)
set(PROJECT_VER "1.0.0.1")

set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/esp_bin/sdkconfig")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/platform"
    "${CMAKE_CURRENT_LIST_DIR}/components/app")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Proje geneli ayarlar (uyarı seti, cJSON iç içe sınırı): idf_build_* project.cmake sonrası tanımlı
include("${CMAKE_CURRENT_LIST_DIR}/cmake/project_settings.cmake")
project(lebensspur)
