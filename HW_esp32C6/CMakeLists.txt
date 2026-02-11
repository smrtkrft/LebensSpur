# LebensSpur ESP32-C6 Projesi
cmake_minimum_required(VERSION 3.16)

# Partition tablosu
set(PARTITION_TABLE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lebensspur)
