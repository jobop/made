idf_component_register(
    SRCS "system_status.cpp"
    INCLUDE_DIRS "include"
    REQUIRES brookesia_core waveshare__esp32_s3_touch_lcd_1_85B
             espressif__i2c_bus vibe_wifi esp_wifi
)
