idf_component_register(SRCS "vibe_theme.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES json esp_http_client nvs_flash spiffs esp_partition heap vibe_usb vibe_pairing)
