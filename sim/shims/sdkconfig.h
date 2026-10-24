/* sim/shims/sdkconfig.h — host 桩。只定义 UI 代码用到的板级选择。
 * 矩形屏（宽高不等，如 240x320）走 2.8 寸分支；方屏（360x360）走默认分支。
 * 尺寸由构建参数 MADE_SIM_H_RES / MADE_SIM_V_RES 决定，与 bsp/display.h 同源。 */
#pragma once

#ifndef MADE_SIM_H_RES
#define MADE_SIM_H_RES 360
#endif
#ifndef MADE_SIM_V_RES
#define MADE_SIM_V_RES 360
#endif

#if MADE_SIM_H_RES != MADE_SIM_V_RES
#define CONFIG_BSP_BOARD_LCD_2_8 1
#else
#define CONFIG_BSP_BOARD_LCD_2_8 0
#endif
