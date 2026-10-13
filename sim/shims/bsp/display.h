/* sim/shims/bsp/display.h — host 桩：
 * 与 build-s3 (CONFIG_BSP_BOARD_LCD_2_8) 一致的 240x320 竖屏。 */
#pragma once

#define BSP_LCD_H_RES 240
#define BSP_LCD_V_RES 320
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
