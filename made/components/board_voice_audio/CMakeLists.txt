idf_component_register(
    SRCS "board_voice_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES waveshare__esp32_s3_touch_lcd_1_85B esp_driver_i2s esp_driver_gpio nvs_flash
)
