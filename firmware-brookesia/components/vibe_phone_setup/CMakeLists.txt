idf_component_register(
    SRCS "vibe_phone_setup.cpp" "phone_setup_protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES vibe_wifi esp_http_server esp_timer esp_hw_support lwip json
)
