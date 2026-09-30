idf_component_register(SRCS "receiver.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server
             esp_driver_usb_serial_jtag mbedtls json esp_timer esp_hw_support
             bootloader_support esp_driver_uart)
