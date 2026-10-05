#pragma once

#include "esp_err.h"
#include "bsp/display.h"
#include "lvgl.h"

// 码得的界面按 360x360 画布排，这套坐标本身就避开了圆屏上下被切掉的区域。
// 宽高相等时整页等比缩放，保持这套安全区，不贴边、不把字拉满。
// 长方形每一行都有整宽，竖屏多出来的高度才留在下面。
constexpr int made_design = 360;

inline int made_screen_w() { return BSP_LCD_H_RES; }
inline int made_screen_h() { return BSP_LCD_V_RES; }
inline bool made_rect() { return made_screen_w() != made_screen_h(); }

inline int made_fit()
{
    const int width = made_screen_w();
    const int height = made_screen_h();
    return width < height ? width : height;
}

inline int made_s(int value) { return value * made_fit() / made_design; }
inline int made_ox() { return (made_screen_w() - made_s(made_design)) / 2; }
inline int made_oy()
{
    const int extra = made_screen_h() - made_s(made_design);
    if (!made_rect() || extra <= 0) return extra / 2;
    return 4;
}
inline int made_x(int value) { return made_ox() + made_s(value); }
inline int made_y(int value) { return made_oy() + made_s(value); }

// 整页（设置、确认框、列表、键盘）按 360 画布的比例铺到真实屏幕高度，
// 原来在方屏里居中的内容，在更高的屏上仍然居中，底下不再空一截。
inline int made_page_x(int value) { return made_x(value); }
inline int made_page_w(int value) { return made_s(value); }
inline int made_page_y(int value)
{
    if (!made_rect()) return made_y(value);
    return value * made_screen_h() / made_design;
}
inline int made_page_h(int value)
{
    if (!made_rect()) return made_s(value);
    const int height = value * made_screen_h() / made_design;
    return height > 0 ? height : (value > 0 ? 1 : 0);
}

inline void made_page_label(lv_obj_t *label, int x, int y, int design_width)
{
    int left = made_x(x);
    int top = made_y(y);
    int box = made_s(design_width);
    if (made_rect()) {
        const int limit = made_screen_w() - 8;
        box = design_width < limit ? design_width : limit;
        const int center = made_page_x(x) + made_page_w(design_width) / 2;
        left = center - box / 2;
        if (left < 4) left = 4;
        if (left + box > made_screen_w() - 4) box = made_screen_w() - 4 - left;
        top = made_page_y(y);
    }
    if (box < 1) box = 1;
    lv_obj_set_pos(label, left, top);
    lv_obj_set_width(label, box);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_scale_x(label, 256, 0);
    lv_obj_set_style_transform_scale_y(label, 256, 0);
}

// LVGL 里 256 表示 100%。图标可以整体缩放，文字必须按字库像素 1:1 画，
// 否则点阵笔画会被插值抹糊。
inline int made_lv_scale() { return 256 * made_fit() / made_design; }

inline void made_text(lv_obj_t *label, int design_width, bool center)
{
    (void)center;
    const int x = lv_obj_get_x(label);
    int width = made_s(design_width);
    const int limit = made_screen_w() - 4;
    if (x >= 0 && x + width > limit) width = limit - x;
    if (width < 1) width = 1;
    lv_obj_set_width(label, width);
    lv_obj_set_style_transform_scale_x(label, 256, 0);
    lv_obj_set_style_transform_scale_y(label, 256, 0);
}
