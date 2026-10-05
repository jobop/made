idf_component_register(
    SRCS "Settings.cpp" "assets/img_app_settings.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core waveshare__esp32_s3_touch_lcd_1_85B
             esp_wifi vibe_wifi system_status vibe_touch_keyboard
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
