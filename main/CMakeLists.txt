idf_component_register(SRCS "app_main.cpp" "device_identity.cpp" "electrical_measurements.cpp" INCLUDE_DIRS "." REQUIRES esp_matter nvs_flash esp_hw_support freertos)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CHIP_HAVE_CONFIG_H DEYE_MODEL_NAME="${FIRMWARE_NAME}")
