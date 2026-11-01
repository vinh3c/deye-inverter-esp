cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Run uv run --python 3.11 scripts/project.py build (see README.md)")
endif()
if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET esp32c3)
endif()
set(PROJECT_VER "0.1.0")
set(PROJECT_VER_NUMBER 1)
file(TO_CMAKE_PATH "$ENV{ESP_MATTER_PATH}" ESP_MATTER_PATH)
set(MATTER_SDK_PATH "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip")
# ESP Insights is pulled in by CHIP's manifest and omits this dependency itself.
set(COMPONENTS main esp_http_client)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# ESP Insights (a CHIP dependency) queries git_describe even in a new repository.
# Its bundled helper cannot read an unborn HEAD; use the release version then.
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    RESULT_VARIABLE git_head_result OUTPUT_QUIET ERROR_QUIET)
if(NOT git_head_result EQUAL 0)
    function(git_describe output_variable)
        set(${output_variable} "${PROJECT_VER}" PARENT_SCOPE)
    endfunction()
endif()
set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components")
# Use the chip target for output names: esp32c3 -> esp32-c3.bin.
string(REGEX REPLACE "^esp32(.+)$" "esp32-\\1" FIRMWARE_NAME "${IDF_TARGET}")
project(${FIRMWARE_NAME})
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
