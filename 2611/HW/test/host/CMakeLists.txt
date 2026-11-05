# LebensSpur 2611 — masaustu birim testleri (IDF bagimsiz saf C moduller).
# Unity 2.6.0 dogrudan IDF agacindan; IDF unity_config / linux hedefi / CMock kullanilmaz.
cmake_minimum_required(VERSION 3.16)
project(ls_host_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()

if(NOT IDF_PATH)
    set(IDF_PATH "$ENV{HOME}/esp/esp-idf-v5.5.2")
endif()
set(HW "${CMAKE_CURRENT_LIST_DIR}/../..")
set(COMMON_FLAGS -Wall -Wextra -Werror -Wshadow)
option(SAN "AddressSanitizer + UBSan" OFF)

add_library(unity STATIC "${IDF_PATH}/components/unity/unity/src/unity.c")
target_include_directories(unity PUBLIC "${IDF_PATH}/components/unity/unity/src")

add_library(cjson STATIC "${IDF_PATH}/components/json/cJSON/cJSON.c")
target_include_directories(cjson PUBLIC "${IDF_PATH}/components/json/cJSON")
target_compile_definitions(cjson PUBLIC CJSON_NESTING_LIMIT=16)

# ls_test(<ad> <test.c> [kaynaklar...]) — kaynaklar HW koku goreli
function(ls_test name test_src)
    set(srcs "${test_src}")
    foreach(s ${ARGN})
        list(APPEND srcs "${HW}/${s}")
    endforeach()
    add_executable(${name} ${srcs})
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/shim"
        "${IDF_PATH}/components/esp_common/include")
    file(GLOB inc_dirs LIST_DIRECTORIES true "${HW}/components/*/*/include")
    target_include_directories(${name} PRIVATE ${inc_dirs})
    target_compile_definitions(${name} PRIVATE LS_HOST_TEST=1)
    target_compile_options(${name} PRIVATE ${COMMON_FLAGS})
    if(SAN)
        target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
    target_link_libraries(${name} PRIVATE unity cjson m)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# cfg semasi: uretec host derlemesinde de calisir
set(CFG_GEN_DIR "${CMAKE_BINARY_DIR}/gen/cfg")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} "${HW}/tools/gen_cfg.py"
                "${HW}/components/platform/cfg/cfg_schema.json" "${CFG_GEN_DIR}"
                RESULT_VARIABLE _res)
if(NOT _res EQUAL 0)
    message(FATAL_ERROR "gen_cfg.py basarisiz")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${HW}/components/platform/cfg/cfg_schema.json" "${HW}/tools/gen_cfg.py")
include_directories("${CFG_GEN_DIR}")

ls_test(test_smoke test_smoke.c)
ls_test(test_cfg_schema test_cfg_schema.c)
target_sources(test_cfg_schema PRIVATE "${CFG_GEN_DIR}/cfg_schema.gen.c")
ls_test(test_cfg_fields test_cfg_fields.c components/platform/cfg/cfg_fields.c)
target_sources(test_cfg_fields PRIVATE "${CFG_GEN_DIR}/cfg_schema.gen.c")
ls_test(test_evlog_classify test_evlog_classify.c components/platform/evlog/evlog_classify.c)
ls_test(test_jobs test_jobs.c components/platform/jobs/jobs.c)
ls_test(test_jsonl_tail test_jsonl_tail.c components/platform/evlog/jsonl_tail.c)
