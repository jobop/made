idf_component_register(
    SRCS "vibe_voice_capture.cpp"
    INCLUDE_DIRS "include"
    REQUIRES vibe_i18n esp_http_client esp-tls board_voice_audio json vibe_pairing vibe_usb
)
