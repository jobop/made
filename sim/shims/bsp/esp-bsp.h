/* sim/shims/bsp/esp-bsp.h — host 桩：真机 BSP 的聚合头。
 * sim/安卓没有真实板级外设，这里只需要让 UI 引用的板级宏可解析：
 * 触摸唤醒脚给占位值（GPIO4），真机由 waveshare BSP 头按板型给出实际引脚。 */
#pragma once

#include "bsp/display.h"

#ifndef BSP_LCD_TOUCH_INT
#define BSP_LCD_TOUCH_INT (GPIO_NUM_4)
#endif
