idf_component_register(
    SRCS "esp_brookesia_app_vibe.cpp" "made_lock_screen.cpp" "new_message_chime.cpp" "new_message_chime_pcm.c" "assets/img_app_vibe.c"
    INCLUDE_DIRS "."
    REQUIRES vibe_i18n brookesia_core esp_http_client esp-tls json esp_wifi esp_timer esp_driver_gpio
             vibe_voice_capture vibe_pairing vibe_wifi vibe_usb vibe_touch_keyboard vibe_phone_setup board_voice_audio xiaozhi-fonts
)
