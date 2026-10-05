idf_component_register(
    SRCS "storage_service.c"
    INCLUDE_DIRS "include"
    REQUIRES waveshare__esp32_s3_touch_lcd_1_85B fatfs esp_hw_support esp_timer
)
