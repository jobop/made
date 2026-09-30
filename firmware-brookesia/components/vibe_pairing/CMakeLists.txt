idf_component_register(
    SRCS "vibe_pairing.cpp"
    INCLUDE_DIRS "include"
    REQUIRES vibe_i18n esp_http_client esp_wifi esp_timer esp_netif esp-tls nvs_flash json lwip mbedtls vibe_wifi vibe_usb
)
