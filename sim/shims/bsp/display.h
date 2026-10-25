/* sim/shims/bsp/display.h — host 桩：
 * 屏幕尺寸运行时由 main() 设置（安卓全屏 / PC 窗口）。
 * 默认 360x360 对齐真机方屏。 */
#pragma once

#ifndef MADE_SIM_H_RES
#define MADE_SIM_H_RES 360
#endif
#ifndef MADE_SIM_V_RES
#define MADE_SIM_V_RES 360
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 运行时屏幕尺寸（main() 启动时根据真实屏幕设置）
extern int g_made_screen_w;
extern int g_made_screen_h;

// 默认值回退到编译期宏
#define BSP_LCD_H_RES (g_made_screen_w ? g_made_screen_w : MADE_SIM_H_RES)
#define BSP_LCD_V_RES (g_made_screen_h ? g_made_screen_h : MADE_SIM_V_RES)
#define BSP_LCD_BITS_PER_PIXEL 16
#define BSP_LCD_COLOR_FORMAT_RGB565 1
#define BSP_LCD_BIGENDIAN 1

int bsp_display_backlight_on(void);
int bsp_display_backlight_off(void);

#ifdef __cplusplus
}
#endif
