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
project(esp32-netlink LANGUAGES C CXX ASM)
