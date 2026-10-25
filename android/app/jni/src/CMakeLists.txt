cmake_minimum_required(VERSION 3.6)
project(made)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL2 target 由上级 jni/CMakeLists.txt 的 add_subdirectory(SDL) 提供
# made 仓库根 = android/app/jni/src 往上 4 级
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../../../deps")

# zlib + libpng（made_sprite 解码 PNG 素材用）
add_subdirectory(${DEPS}/zlib-1.3.1 ${CMAKE_BINARY_DIR}/zlib)
set(ZLIB_FOUND ON CACHE BOOL "" FORCE)
set(ZLIB_LIBRARIES zlib CACHE STRING "" FORCE)
set(ZLIB_INCLUDE_DIRS ${DEPS}/zlib-1.3.1 CACHE STRING "" FORCE)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/libpng-1.6.43 ${CMAKE_BINARY_DIR}/libpng)
include_directories(${DEPS}/libpng-1.6.43 ${CMAKE_BINARY_DIR}/libpng)
set(MADE "${ROOT}/made")
set(LVGL "${MADE}/managed_components/lvgl__lvgl")
set(COMP "${MADE}/components")
set(SHIMS "${ROOT}/sim/shims")
set(STUBS "${ROOT}/sim/stubs")
set(CJSON "${MADE}/managed_components/espressif__cjson/cJSON")
set(FONTS "${MADE}/managed_components/78__xiaozhi-fonts/src")

add_definitions(
  -DLV_CONF_INCLUDE_SIMPLE
  -DLV_LVGL_H_INCLUDE_SIMPLE
  -DLV_FONT_FMT_TXT_LARGE=1
  -DMADE_SIM_H_RES=360
  -DMADE_SIM_V_RES=360
  -DSO_NOSIGPIPE=0
)

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}/../compat"
  "${ROOT}/sim"
  "${SHIMS}"
  "${SHIMS}/bsp"
  "${LVGL}"
  "${LVGL}/src"
  "${COMP}/brookesia_app_vibe"
  "${COMP}/vibe_touch_keyboard"
  "${COMP}/vibe_i18n/include"
  "${COMP}/vibe_theme/include"
  "${COMP}/vibe_wifi/include"
  "${COMP}/vibe_pairing/include"
  "${COMP}/vibe_phone_setup/include"
  "${COMP}/vibe_usb/include"
  "${COMP}/vibe_voice_capture/include"
  "${COMP}/board_voice_audio/include"
  "${COMP}/vibe_motion/include"
  "${CJSON}"
  "${FONTS}"
)

file(GLOB_RECURSE LVGL_SRCS "${LVGL}/src/*.c")

set(APP_SRCS
  main.cpp
  ${COMP}/brookesia_app_vibe/esp_brookesia_app_vibe.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_ui.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_render.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_screenshot.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_transport.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_attachments.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_board_events.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_commands.cpp
  ${COMP}/brookesia_app_vibe/vibe_ui_shared.cpp
  ${COMP}/brookesia_app_vibe/vibe_settings_pages.cpp
  ${COMP}/brookesia_app_vibe/vibe_artwork.cpp
  ${COMP}/brookesia_app_vibe/vibe_backend.cpp
  ${COMP}/brookesia_app_vibe/made_sprite.cpp
  ${COMP}/brookesia_app_vibe/made_sprite_cache.cpp
  ${COMP}/brookesia_app_vibe/made_gif.cpp
  ${COMP}/brookesia_app_vibe/assets/img_app_vibe.c
  ${COMP}/brookesia_app_vibe/assets/img_sprite_bean.c
  ${COMP}/vibe_touch_keyboard/vibe_touch_keyboard.cpp
  ${COMP}/vibe_i18n/vibe_i18n.cpp
  ${COMP}/vibe_theme/vibe_theme.cpp
  ${MADE}/managed_components/espressif__cjson/cJSON/cJSON.c
  ${COMP}/brookesia_app_vibe/fonts/font_puhui_14_4.c
  ${FONTS}/font_puhui_16_4.c
  ${FONTS}/font_puhui_20_4.c
  ${STUBS}/esp_stubs.cpp
  ${STUBS}/components_stubs.cpp
)

add_library(main SHARED ${LVGL_SRCS} ${APP_SRCS})

# 与 sim 一致：lv_compat.h 只注入 C++ 源文件（补 LVGL API 的 C++ 重载），
# 不能给 LVGL 的 .c 文件用（会 conflicting types）。
set(CPP_SRCS
  main.cpp
  ${COMP}/brookesia_app_vibe/esp_brookesia_app_vibe.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_ui.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_render.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_screenshot.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_transport.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_attachments.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_board_events.cpp
  ${COMP}/brookesia_app_vibe/vibe_app_commands.cpp
  ${COMP}/brookesia_app_vibe/vibe_ui_shared.cpp
  ${COMP}/brookesia_app_vibe/vibe_settings_pages.cpp
  ${COMP}/brookesia_app_vibe/vibe_artwork.cpp
  ${COMP}/brookesia_app_vibe/vibe_backend.cpp
  ${COMP}/brookesia_app_vibe/made_sprite.cpp
  ${COMP}/brookesia_app_vibe/made_sprite_cache.cpp
  ${COMP}/brookesia_app_vibe/made_gif.cpp
  ${COMP}/vibe_touch_keyboard/vibe_touch_keyboard.cpp
  ${COMP}/vibe_i18n/vibe_i18n.cpp
  ${COMP}/vibe_theme/vibe_theme.cpp
  ${STUBS}/esp_stubs.cpp
  ${STUBS}/components_stubs.cpp
)
set_source_files_properties(${CPP_SRCS} PROPERTIES
  COMPILE_OPTIONS "-include;${SHIMS}/lv_compat.h")

target_link_libraries(main
  SDL2
  png_static
  zlib
  android
  log
  GLESv2
)
