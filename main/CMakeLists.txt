set(board_sources)
set(board_dependencies)
if(FIRMWARE_TARGET STREQUAL "sonoff-basic-r4")
    list(APPEND board_sources "sonoff_relay.cpp")
    list(APPEND board_dependencies esp_driver_gpio)
endif()
set(profile "${CMAKE_CURRENT_LIST_DIR}/../profiles/deye-single.json")
set(generator "${CMAKE_CURRENT_LIST_DIR}/../scripts/generate_registers.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${profile}" "${generator}")
idf_component_register(SRCS "app_main.cpp" "device_identity.cpp" "electrical_measurements.cpp" "logger_config.cpp" "logger_web.cpp" "inverter_data.cpp" ${board_sources}
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}" REQUIRES esp_matter nvs_flash esp_hw_support freertos lwip esp_timer esp_http_server esp_netif ${board_dependencies}
    EMBED_TXTFILES "logger_config.html")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${generator}" "${profile}" "${CMAKE_CURRENT_BINARY_DIR}/deye_registers.h"
    RESULT_VARIABLE register_generation_result)
if(NOT register_generation_result EQUAL 0)
    message(FATAL_ERROR "Cannot generate Deye register definitions")
endif()

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CHIP_HAVE_CONFIG_H DEYE_MODEL_NAME="${FIRMWARE_NAME}")
if(FIRMWARE_TARGET STREQUAL "sonoff-basic-r4")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEYE_SONOFF_BASIC_R4=1)
endif()
