cmake_minimum_required(VERSION 3.16)

# Extract firmware version from git tag.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    TIMEOUT 5
)
if("${GIT_VERSION}" STREQUAL "")
    set(GIT_VERSION "v0.1.0-dev")
endif()
message(STATUS "Firmware version: ${GIT_VERSION}")
add_compile_definitions(FIRMWARE_VERSION="${GIT_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-taplink LANGUAGES C CXX ASM)

idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(
    ${lwip_lib}
    PRIVATE ESP_IDF_LWIP_HOOK_FILENAME="taplink_lwip_hooks.h"
)
target_include_directories(${lwip_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main")
