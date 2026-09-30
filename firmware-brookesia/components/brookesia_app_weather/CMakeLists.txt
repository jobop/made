idf_component_register(
    SRCS "esp_brookesia_app_weather.cpp" "weather_data.cpp" "assets/icon_weather.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core esp_http_client esp_wifi mbedtls nvs_flash json
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
