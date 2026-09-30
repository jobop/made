idf_component_register(
    SRCS "vibe_wifi.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif esp_timer esp_wifi nvs_flash
)
