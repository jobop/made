// SPDX-License-Identifier: Apache-2.0
#include "made_lock_screen.hpp"

#include <algorithm>
#include <new>

LV_IMAGE_DECLARE(img_app_vibe);
LV_FONT_DECLARE(font_puhui_16_4);

namespace made_lock_screen {
namespace {
constexpr uint32_t kBackground = 0x0B1518;
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
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
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
    lv_obj_set_style_line_width(object, width, 0);
    lv_obj_set_style_line_rounded(object, true, 0);
    return object;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width,
                const lv_font_t *font, uint32_t color) {
    auto *object = lv_label_create(parent);
    passive(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
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
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 360, 360);
    lv_obj_set_style_bg_color(root, lv_color_hex(kBackground), 0);
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

    // A soft stage and a small brand mark leave the character the main focus.
    auto *halo = shape(root, 81, 48, 198, 198, 0x12282B);
    lv_obj_set_style_bg_grad_color(halo, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_grad_dir(halo, LV_GRAD_DIR_VER, 0);
    auto *brand = label(root, "made", 125, 27, 110, &lv_font_montserrat_16, kLime);
    lv_obj_set_style_text_letter_space(brand, 3, 0);
    parts->shadow = shape(root, 139, 214, 82, 9, 0x5B846D, 55);
    parts->left_arm = line(root, 0xABD885, 6);
    parts->right_arm = line(root, 0xABD885, 6);
    parts->left_foot = shape(root, 150, 196, 18, 8, kLime);
    parts->right_foot = shape(root, 194, 196, 18, 8, kLime);

    // Crop the approved icon's frame away with an LVGL circular clip. The
    // original bean/face pixels remain intact; the limbs supply a dancing pose.
    parts->body = shape(root, 112, 64, 136, 136, 0x102629);
    lv_obj_set_style_clip_corner(parts->body, true, 0);
    parts->image = lv_image_create(parts->body);
    passive(parts->image);
    lv_image_set_src(parts->image, &img_app_vibe);
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
    lv_obj_set_pos(parts->arrow, 173, 294);
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
    lv_obj_set_pos(parts->body, 112 + sway, 64 - bounce);
    // Five degrees of rocking, never a spinning square.
    const int angle = wave(elapsed_ms, 1800, 45);
    lv_image_set_rotation(parts->image, angle < 0 ? angle + 3600 : angle);
    lv_obj_set_width(parts->shadow, 82 - bounce * 2);
    lv_obj_set_x(parts->shadow, 139 + bounce + sway / 2);
    lv_obj_set_style_bg_opa(parts->shadow, 55 - bounce * 2, 0);

    parts->left_points[0] = {125 + sway, 145 - bounce};
    parts->left_points[1] = {106 + sway, 134 - bounce};
    parts->left_points[2] = {100 + sway, 114 - step * 2 - bounce};
    parts->right_points[0] = {234 + sway, 142 - bounce};
    parts->right_points[1] = {251 + sway, 132 - bounce};
    parts->right_points[2] = {260 + sway, 115 + step * 2 - bounce};
    lv_line_set_points(parts->left_arm, parts->left_points, 3);
    lv_line_set_points(parts->right_arm, parts->right_points, 3);
    lv_obj_set_pos(parts->left_foot, 150 + sway - step, 196 - std::max(0, step) - bounce / 2);
    lv_obj_set_pos(parts->right_foot, 194 + sway - step, 196 - std::max(0, -step) - bounce / 2);
    lv_obj_set_pos(parts->note[0], 78 + wave(elapsed_ms, 2700, 3), 81 + wave(elapsed_ms, 2400, 7));
    lv_obj_set_pos(parts->note[1], 263 + wave(elapsed_ms, 2600, 3), 160 + wave(elapsed_ms, 2500, 7, 180));
    for (int i = 0; i < 3; ++i)
        lv_obj_set_style_bg_opa(parts->spark[i], 135 + wave(elapsed_ms, 2100, 50, i * 100), 0);

    const bool slogan_english = english != ((elapsed_ms / kSloganPeriod) % 2 != 0);
    if (!parts->initialized || slogan_english != parts->last_slogan_english) {
        lv_obj_set_style_text_font(parts->slogan, slogan_english ? &lv_font_montserrat_18 : &font_puhui_16_4, 0);
        // Reuse the full Chinese font already in flash at a readable 20 px.
        // Centered native scaling avoids adding another large font resource.
        const int scale = slogan_english ? LV_SCALE_NONE : LV_SCALE_NONE * 5 / 4;
        lv_obj_set_style_transform_scale_x(parts->slogan, scale, 0);
        lv_obj_set_style_transform_scale_y(parts->slogan, scale, 0);
        lv_label_set_text(parts->slogan, slogan_english ? "Made,#C7F58B when you want to make#" : "码得，#C7F58B 代码触手可得#");
        parts->last_slogan_english = slogan_english;
    }
    if (!parts->initialized || english != parts->last_hint_english) {
        lv_obj_set_style_text_font(parts->hint, english ? &lv_font_montserrat_16 : &font_puhui_16_4, 0);
        lv_label_set_text(parts->hint, english ? "Swipe up" : "上滑进入");
        parts->last_hint_english = english;
    }
    const uint32_t phase = elapsed_ms % kSloganPeriod;
    const int opacity = phase < 220 ? 110 + phase * 145 / 220 :
        phase > kSloganPeriod - 220 ? 110 + (kSloganPeriod - phase) * 145 / 220 : 255;
    lv_obj_set_style_text_opa(parts->slogan, static_cast<lv_opa_t>(opacity), 0);
    lv_obj_set_y(parts->arrow, 294 + wave(elapsed_ms, 1800, 3));
    parts->initialized = true;
}
} // namespace made_lock_screen
