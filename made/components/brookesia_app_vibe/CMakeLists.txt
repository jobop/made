idf_component_register(
    SRCS "esp_brookesia_app_vibe.cpp" "made_sprite.cpp" "made_sprite_cache.cpp" "new_message_chime.cpp" "new_message_chime_pcm.c" "assets/img_app_vibe.c" "fonts/font_puhui_14_4.c" "fonts/font_puhui_16_4.c"
    INCLUDE_DIRS "."
    REQUIRES vibe_i18n brookesia_core esp_http_client esp-tls json esp_wifi esp_timer esp_driver_gpio app_update
             vibe_voice_capture vibe_pairing vibe_wifi vibe_theme vibe_usb vibe_touch_keyboard vibe_phone_setup board_voice_audio xiaozhi-fonts
             espressif__libpng mbedtls
)

# 16px 点阵改由本组件提供压缩版。xiaozhi-fonts 在 managed_components 里，
# 重新下载会带回未压缩的同名符号，这里把它从那个库的编译列表拿掉。
idf_build_get_property(build_components BUILD_COMPONENTS)
foreach(component IN LISTS build_components)
    if(component MATCHES "xiaozhi-fonts")
        idf_component_get_property(font_lib ${component} COMPONENT_LIB)
        get_target_property(font_srcs ${font_lib} SOURCES)
        set(filtered_srcs "")
        foreach(src IN LISTS font_srcs)
            if(NOT src MATCHES "font_puhui_(16|20)_4\\.c$")
                list(APPEND filtered_srcs "${src}")
            endif()
        endforeach()
        set_target_properties(${font_lib} PROPERTIES SOURCES "${filtered_srcs}")
    endif()
endforeach()
