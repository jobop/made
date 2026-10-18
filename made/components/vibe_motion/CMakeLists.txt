idf_component_register(
    SRCS "vibe_motion.cpp"
    INCLUDE_DIRS "include"
    REQUIRES waveshare__esp32_s3_touch_lcd_1_85B waveshare__qmi8658 esp_driver_i2c esp_timer
)
