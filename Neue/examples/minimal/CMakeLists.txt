cmake_minimum_required(VERSION 3.16)

# Point the build at the skapp-library component tree (three levels up).
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sk_minimal)
