/**
 * sim/lv_conf.h — 模拟器专用 LVGL 配置。
 * 只写与固件 (build-s3 sdkconfig) 不同的覆盖项；其余走 lv_conf_internal 默认。
 * 固件设置镜像：COLOR_DEPTH 16 / LV_USE_OS none / montserrat 8..26。
 */
#pragma once

/* ----------------Display & Draw---------------- */
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_DISPLAY_DEF_REFR_PERIOD 33

/* 固件侧 UI 访问 lv_timer_t 成员等私有 API（brookesia 链路需要）。 */
#define LV_USE_PRIVATE_API 1
#define LV_USE_QRCODE 1
#define LV_USE_SNAPSHOT 1

/* ----------------SDL 模拟窗口---------------- */
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH "SDL2/SDL.h"

/* ----------------Fonts（与固件 sdkconfig 对齐）---------------- */
#define LV_FONT_MONTSERRAT_8 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_DEFAULT &lv_font_montserrat_18

/* ----------------FreeType 矢量字（sim/安卓运行时按屏幕等比缩放）----------------
 * 真机固件走点阵字（见 sdkconfig），这里只在宿主/安卓用 TTF 渲染任意字号。
 * USE_LVGL_PORT=0：直接用平台 stdio 读 TTF 文件路径，不接 LVGL FS 抽象。 */
#define LV_USE_FREETYPE 1
#define LV_FREETYPE_USE_LVGL_PORT 0
#define LV_FREETYPE_CACHE_FT_GLYPH_CNT 256

/* ----------------调试---------------- */
#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0
