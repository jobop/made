// SPDX-License-Identifier: Apache-2.0
#include "made_lock_screen.hpp"
#include "vibe_theme.hpp"
#include "made_layout.hpp"

#include <algorithm>
#include <new>

LV_IMAGE_DECLARE(img_app_vibe);
LV_FONT_DECLARE(font_puhui_16_4);

namespace made_lock_screen {
namespace {
constexpr uint32_t kBackground = 0x0B1518;  // 默认值；运行时取 vibe_theme::palette().lock_bg
constexpr uint32_t kLime = 0xC7F58B;
constexpr uint32_t kWhite = 0xF0F5EE;
constexpr uint32_t kMuted = 0x99B3B1;
constexpr uint32_t kSloganPeriod = 5000;

struct Parts {
    lv_obj_t *body = nullptr;
    lv_obj_t *image = nullptr;
    lv_obj_t *shadow = nullptr;
    lv_obj_t *left_arm = nullptr;
    lv_obj_t *right_arm = nullptr;
    lv_obj_t *left_foot = nullptr;
    lv_obj_t *right_foot = nullptr;
    lv_obj_t *note[2]{};
    lv_obj_t *spark[3]{};
    lv_obj_t *brand = nullptr;
    lv_obj_t *slogan = nullptr;
    lv_obj_t *hint = nullptr;
    lv_obj_t *arrow = nullptr;
    lv_point_precise_t left_points[3]{};
    lv_point_precise_t right_points[3]{};
    lv_point_precise_t arrow_points[3]{{0, 7}, {7, 0}, {14, 7}};
    bool initialized = false;
    bool last_slogan_english = false;
    bool last_hint_english = false;
};

void passive(lv_obj_t *object) {
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
}

lv_obj_t *shape(lv_obj_t *parent, int x, int y, int width, int height,
                uint32_t color, lv_opa_t opacity = LV_OPA_COVER) {
    auto *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    passive(object);
    lv_obj_set_pos(object, made_x(x), made_y(y));
    lv_obj_set_size(object, made_s(width), made_s(height));
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    return object;
}

lv_obj_t *line(lv_obj_t *parent, uint32_t color, int width) {
    auto *object = lv_line_create(parent);
    lv_obj_remove_style_all(object);
    passive(object);
    lv_obj_set_style_line_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(object, made_s(width) > 0 ? made_s(width) : 1, 0);
    lv_obj_set_style_line_rounded(object, true, 0);
    return object;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width,
                const lv_font_t *font, uint32_t color) {
    auto *object = lv_label_create(parent);
    passive(object);
    lv_obj_set_pos(object, made_x(x), made_y(y));
    lv_obj_set_style_text_font(object, font, 0);
    made_text(object, width, false);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(object, 5, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_label_set_text(object, text);
    return object;
}

int wave(uint32_t elapsed_ms, uint32_t period, int amplitude, int phase = 0) {
    const int angle = static_cast<int>((elapsed_ms % period) * 360 / period);
    return lv_trigo_sin((angle + phase) % 360) * amplitude / LV_TRIGO_SIN_MAX;
}

void destroy(lv_event_t *event) {
    auto *parts = static_cast<Parts *>(lv_event_get_user_data(event));
    lv_obj_set_user_data(lv_event_get_target_obj(event), nullptr);
    delete parts;
}
} // namespace

lv_obj_t *create(lv_obj_t *parent, lv_event_cb_t swipe_callback, void *user_data) {
    auto *parts = new (std::nothrow) Parts;
    if (!parts) return nullptr;
    auto *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, made_s(0), made_s(0));
    lv_obj_set_size(root, made_screen_w(), made_screen_h());
    lv_obj_set_style_bg_color(root, lv_color_hex(vibe_theme::palette().lock_bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(root, parts);
    lv_obj_add_event_cb(root, destroy, LV_EVENT_DELETE, parts);
    if (swipe_callback) {
        lv_obj_add_event_cb(root, swipe_callback, LV_EVENT_GESTURE, user_data);
        lv_obj_add_event_cb(root, swipe_callback, LV_EVENT_PRESSED, user_data);
        lv_obj_add_event_cb(root, swipe_callback, LV_EVENT_RELEASED, user_data);
        lv_obj_add_event_cb(root, swipe_callback, LV_EVENT_PRESS_LOST, user_data);
    }

    const bool theme_icon = vibe_theme::lockAsset().has_icon;
    // A soft stage and a small brand mark leave the character the main focus.
    auto *halo = shape(root, 81, 48, 198, 198, 0x12282B);
    lv_obj_set_style_bg_grad_color(halo, lv_color_hex(vibe_theme::palette().lock_bg), 0);
    lv_obj_set_style_bg_grad_dir(halo, LV_GRAD_DIR_VER, 0);
    parts->brand = label(root, "made", 125, 27, 110, &lv_font_montserrat_16, kLime);
    lv_obj_set_style_text_letter_space(parts->brand, 3, 0);
    if (!theme_icon) {
        parts->shadow = shape(root, 139, 214, 82, 9, 0x5B846D, 55);
        parts->left_arm = line(root, 0xABD885, 6);
        parts->right_arm = line(root, 0xABD885, 6);
        parts->left_foot = shape(root, 150, 196, 18, 8, kLime);
        parts->right_foot = shape(root, 194, 196, 18, 8, kLime);
    }

    // Crop the approved icon's frame away with an LVGL circular clip. The
    // original bean/face pixels remain intact; the limbs supply a dancing pose.
    parts->body = shape(root, 112, 64, 136, 136, 0x102629);
    // Keep the circle in design pixels so the 112 px artwork is cropped the
    // same way as on the round screen, then scale the finished circle.
    lv_obj_set_size(parts->body, 136, 136);
    lv_obj_set_style_transform_pivot_x(parts->body, 0, 0);
    lv_obj_set_style_transform_pivot_y(parts->body, 0, 0);
    lv_obj_set_style_transform_scale_x(parts->body, made_lv_scale(), 0);
    lv_obj_set_style_transform_scale_y(parts->body, made_lv_scale(), 0);
    lv_obj_set_style_clip_corner(parts->body, true, 0);
    parts->image = lv_image_create(parts->body);
    passive(parts->image);
    if (theme_icon) {
        const auto &asset = vibe_theme::lockAsset();
        static lv_image_dsc_t theme_icon_dsc = {};
        theme_icon_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        theme_icon_dsc.header.w = asset.icon_w;
        theme_icon_dsc.header.h = asset.icon_h;
        theme_icon_dsc.data = asset.icon_data;
        theme_icon_dsc.data_size = static_cast<uint32_t>(asset.icon_w) * asset.icon_h * 2;
        lv_image_set_src(parts->image, &theme_icon_dsc);
    } else {
        lv_image_set_src(parts->image, &img_app_vibe);
    }
    lv_obj_set_pos(parts->image, 12, 12);
    lv_image_set_pivot(parts->image, 56, 56);
    lv_image_set_scale(parts->image, 368);
    lv_image_set_antialias(parts->image, true);

    // Tiny musical notes use native shapes, avoiding unsupported font glyphs.
    for (int i = 0; i < 2; ++i) {
        parts->note[i] = shape(root, 0, 0, 20, 29, kBackground, LV_OPA_TRANSP);
        shape(parts->note[i], 1, 20, 10, 7, i ? kLime : 0x78CAC4);
        auto *stem = shape(parts->note[i], 8, 2, 3, 23, i ? kLime : 0x78CAC4);
        lv_obj_set_style_radius(stem, 1, 0);
        auto *flag = shape(parts->note[i], 9, 2, 9, 3, i ? kLime : 0x78CAC4);
        lv_obj_set_style_radius(flag, 1, 0);
    }
    parts->spark[0] = shape(root, 83, 157, 4, 4, kLime, 160);
    parts->spark[1] = shape(root, 268, 117, 4, 4, kLime, 190);
    parts->spark[2] = shape(root, 239, 57, 3, 3, 0x87CEC1, 130);

    parts->slogan = label(root, "码得，代码触手可得", 36, 239, 288,
                          &font_puhui_16_4, kWhite);
    lv_label_set_recolor(parts->slogan, true);
    lv_obj_set_style_transform_pivot_x(parts->slogan, 144, 0);
    lv_obj_set_style_transform_pivot_y(parts->slogan, 0, 0);
    parts->arrow = line(root, kLime, 2);
    lv_line_set_points(parts->arrow, parts->arrow_points, 3);
    lv_obj_set_pos(parts->arrow, made_x(173), made_y(294));
    parts->hint = label(root, "上滑进入", 89, 313, 182, &font_puhui_16_4, kMuted);
    update(root, 0, false);
    return root;
}

void update(lv_obj_t *root, uint32_t elapsed_ms, bool english) {
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts) return;
    const int sway = wave(elapsed_ms, 1800, 7);
    const int bounce = (wave(elapsed_ms, 900, 8, 270) + 8) / 2;
    const int step = wave(elapsed_ms, 1800, 4);
    lv_obj_set_pos(parts->body, made_x(112 + sway), made_y(64 - bounce));
    // Five degrees of rocking, never a spinning square.
    const int angle = wave(elapsed_ms, 1800, 45);
    lv_image_set_rotation(parts->image, angle < 0 ? angle + 3600 : angle);
    lv_obj_set_width(parts->shadow, made_s(82 - bounce * 2));
    lv_obj_set_x(parts->shadow, made_x(139 + bounce + sway / 2));
    lv_obj_set_style_bg_opa(parts->shadow, 55 - bounce * 2, 0);

    parts->left_points[0] = {made_x(125 + sway), made_y(145 - bounce)};
    parts->left_points[1] = {made_x(106 + sway), made_y(134 - bounce)};
    parts->left_points[2] = {made_x(100 + sway), made_y(114 - step * 2 - bounce)};
    parts->right_points[0] = {made_x(234 + sway), made_y(142 - bounce)};
    parts->right_points[1] = {made_x(251 + sway), made_y(132 - bounce)};
    parts->right_points[2] = {made_x(260 + sway), made_y(115 + step * 2 - bounce)};
    lv_line_set_points(parts->left_arm, parts->left_points, 3);
    lv_line_set_points(parts->right_arm, parts->right_points, 3);
    lv_obj_set_pos(parts->left_foot, made_x(150 + sway - step), made_y(196 - std::max(0, step) - bounce / 2));
    lv_obj_set_pos(parts->right_foot, made_x(194 + sway - step), made_y(196 - std::max(0, -step) - bounce / 2));
    lv_obj_set_pos(parts->note[0], made_x(78 + wave(elapsed_ms, 2700, 3)), made_y(81 + wave(elapsed_ms, 2400, 7)));
    lv_obj_set_pos(parts->note[1], made_x(263 + wave(elapsed_ms, 2600, 3)), made_y(160 + wave(elapsed_ms, 2500, 7, 180)));
    for (int i = 0; i < 3; ++i)
        lv_obj_set_style_bg_opa(parts->spark[i], 135 + wave(elapsed_ms, 2100, 50, i * 100), 0);

    const bool slogan_english = english != ((elapsed_ms / kSloganPeriod) % 2 != 0);
    if (!parts->initialized || slogan_english != parts->last_slogan_english) {
        lv_obj_set_style_text_font(parts->slogan, slogan_english ? &lv_font_montserrat_18 : &font_puhui_16_4, 0);
        if (made_fit() >= made_design) {
            // Reuse the full Chinese font already in flash at a readable 20 px.
            const int scale = slogan_english ? 256 : 320;
            lv_obj_set_style_transform_pivot_x(parts->slogan, 144, 0);
            lv_obj_set_x(parts->slogan, made_x(180) - 144);
            lv_obj_set_style_transform_scale_x(parts->slogan, scale, 0);
            lv_obj_set_style_transform_scale_y(parts->slogan, scale, 0);
        }
        lv_label_set_text(parts->slogan, slogan_english ? "Made,#C7F58B when you want to make#" : "码得，#C7F58B 代码触手可得#");
        parts->last_slogan_english = slogan_english;
    }
    if (!parts->initialized || english != parts->last_hint_english) {
        lv_obj_set_style_text_font(parts->hint, english ? &lv_font_montserrat_16 : &font_puhui_16_4, 0);
        lv_label_set_text(parts->hint, english ? "Swipe up" : "上滑进入");
        parts->last_hint_english = english;
    }
    if (made_rect() && made_fit() < made_design) {
        // Full-width centered lines. The hint sits under the wrapped slogan,
        // so "make" and "上滑进入" no longer share the same row.
        const int margin = 12;
        const int text_w = made_screen_w() - margin * 2;
        const int text_y = made_y(64) + made_s(136) + 16;
        lv_obj_set_width(parts->slogan, text_w);
        lv_obj_set_pos(parts->slogan, margin, text_y);
        lv_obj_set_style_text_align(parts->slogan, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_transform_scale_x(parts->slogan, 256, 0);
        lv_obj_set_style_transform_scale_y(parts->slogan, 256, 0);
        lv_obj_update_layout(parts->slogan);
        const int slogan_h = lv_obj_get_height(parts->slogan);
        const int hint_y = text_y + (slogan_h > 0 ? slogan_h : 20) + 10;
        lv_obj_set_width(parts->hint, text_w);
        lv_obj_set_pos(parts->hint, margin, hint_y);
        lv_obj_set_style_text_align(parts->hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_transform_scale_x(parts->hint, 256, 0);
        lv_obj_set_style_transform_scale_y(parts->hint, 256, 0);
        if (parts->brand) {
            lv_obj_set_width(parts->brand, text_w);
            lv_obj_set_x(parts->brand, margin);
            lv_obj_set_style_text_align(parts->brand, LV_TEXT_ALIGN_CENTER, 0);
        }
        lv_obj_set_x(parts->arrow, (made_screen_w() - 14) / 2);
        lv_obj_set_y(parts->arrow, hint_y + 28);
    }
    const uint32_t phase = elapsed_ms % kSloganPeriod;
    const int opacity = phase < 220 ? 110 + phase * 145 / 220 :
        phase > kSloganPeriod - 220 ? 110 + (kSloganPeriod - phase) * 145 / 220 : 255;
    lv_obj_set_style_text_opa(parts->slogan, static_cast<lv_opa_t>(opacity), 0);
    if (made_fit() >= made_design) {
        lv_obj_set_y(parts->arrow, made_y(294 + wave(elapsed_ms, 1800, 3)));
    }
    parts->initialized = true;
}
} // namespace made_lock_screen
