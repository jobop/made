# android/app/src/main/jni/CMakeLists.txt
#
# 把 made 的 UI 应用层 + LVGL + ESP-IDF 桩，编译成安卓 .so，
# 由 SDL2-Android 加载并显示。源文件清单照搬 sim/Makefile，
# 这样「PC sim 能编，安卓就能编」。

cmake_minimum_required(VERSION 3.22)
project(made_android LANGUAGES C CXX)

# ---- 仓库相对路径（本文件在 android/app/src/main/jni/） ----
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(MADE "${ROOT}/made")
set(LVGL "${MADE}/managed_components/lvgl__lvgl")
set(COMP "${MADE}/components")
set(SHIMS "${ROOT}/sim/shims")
set(STUBS "${ROOT}/sim/stubs")
set(CJSON "${MADE}/managed_components/espressif__cjson/cJSON")
set(FONTS "${MADE}/managed_components/78__xiaozhi-fonts/src")

# 安卓默认竖屏基准分辨率；SDL2 全屏后由真实窗口尺寸驱动，
# made_layout.hpp 会按真实短边自适应，这里只给 BSP_LCD_* 一个定义。
add_definitions(
  -DLV_CONF_INCLUDE_SIMPLE
  -DLV_LVGL_H_INCLUDE_SIMPLE
  -DLV_FONT_FMT_TXT_LARGE=1
  -DMADE_SIM_H_RES=360
  -DMADE_SIM_V_RES=640
)

# ---- 头文件搜索（照搬 sim COMMON_INC） ----
include_directories(
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

# ---- LVGL 全部源码 ----
file(GLOB_RECURSE LVGL_SRCS "${LVGL}/src/*.c")

# ---- made 应用层（照搬 sim APP_SRCS） ----
set(APP_SRCS
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
  android_main.cpp
)

add_library(made SHARED ${LVGL_SRCS} ${APP_SRCS})

# 安卓 SDL2 由 Gradle 提供；链接 SDL 与 log/android 系统库
find_library(SDL2 SDL2)
target_link_libraries(made
  ${SDL2}
  android
  log
)
