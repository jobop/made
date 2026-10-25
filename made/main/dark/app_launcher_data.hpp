/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "bsp/display.h"
#include "systems/phone/widgets/app_launcher/esp_brookesia_app_launcher.hpp"

// Reuse the Chinese font already linked for the round-screen app.
LV_FONT_DECLARE(font_puhui_16_4);

namespace esp_brookesia::systems::phone {

// 桌面网格自适应：运行时按屏幕形状取布局档位，不做板级编译分支。
//  - 方屏（宽=高，360 画布设计值）：4 列大图标；
//  - 矩形竖屏（如 240x320）：收成 3 列整宽小图标。
// 两档数值均为设计稿调好的定值，屏幕尺寸由 made_layout 的短边缩放另行处理。
inline bool made_launcher_tall_screen()
{
    return BSP_LCD_V_RES > BSP_LCD_H_RES;
}

inline AppLauncherIcon::Data made_launcher_icon_data()
{
    const bool tall = made_launcher_tall_screen();
    return {
        .main = {
            .size = gui::StyleSize::SQUARE(tall ? 72 : 120),
            .layout_row_pad = tall ? 2 : 5, // 95 px icon + 5 px gap + 20 px Chinese line fits 120 px.
        },
        .image = {
            .default_size = gui::StyleSize::SQUARE(tall ? 52 : 95),
            .press_size = gui::StyleSize::SQUARE(tall ? 44 : 80),
        },
        .label = {
            .text_font = gui::StyleFont::CUSTOM_SIZE(16, &font_puhui_16_4),
            .text_color = gui::StyleColor::COLOR(0xFFFFFF),
        },
    };
}

inline AppLauncherData made_launcher_data()
{
    const bool tall = made_launcher_tall_screen();
    return {
        .main = {
            .y_start = 0,
            .size = gui::StyleSize::RECT_PERCENT(100, 100),
        },
        .table = {
            .default_num = tall ? 3 : 4,
            .size = tall ? gui::StyleSize::RECT_PERCENT(100, 78)
                         : gui::StyleSize::RECT_PERCENT(80, 70),
        },
        .indicator = {
            .main_size = gui::StyleSize::RECT_W_PERCENT(100, 20),
            .main_layout_column_pad = 30,
            .main_layout_bottom_offset = 30,
            .spot_inactive_size = gui::StyleSize::SQUARE(12),
            .spot_active_size = gui::StyleSize::RECT(40, 12),
            .spot_inactive_background_color = gui::StyleColor::COLOR(0xC6C6C6),
            .spot_active_background_color = gui::StyleColor::COLOR(0xFFFFFF),
        },
        .icon = made_launcher_icon_data(),
        .flags = {
            .enable_table_scroll_anim = 0,
        },
    };
}

} // namespace esp_brookesia::systems::phone
