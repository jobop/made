/* sim/shims/bsp/display.h — host 桩：
 * 屏幕尺寸由构建参数 MADE_SIM_H_RES / MADE_SIM_V_RES 决定（make SCREEN=宽x高），
 * 默认对齐真机方屏 360x360；make SCREEN=240x320 切矩形竖屏。 */
#pragma once

#ifndef MADE_SIM_H_RES
#define MADE_SIM_H_RES 360
#endif
#ifndef MADE_SIM_V_RES
#define MADE_SIM_V_RES 360
#endif

#define BSP_LCD_H_RES MADE_SIM_H_RES
#define BSP_LCD_V_RES MADE_SIM_V_RES
#define BSP_LCD_BITS_PER_PIXEL 16
#define BSP_LCD_COLOR_FORMAT_RGB565 1
#define BSP_LCD_BIGENDIAN 1

#ifdef __cplusplus
extern "C" {
#endif
int bsp_display_backlight_on(void);
int bsp_display_backlight_off(void);
#ifdef __cplusplus
}
#endif
