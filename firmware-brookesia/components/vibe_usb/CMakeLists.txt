idf_component_register(
    SRCS "vibe_usb.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_usb_serial_jtag esp_hw_support esp_timer json mbedtls
)
