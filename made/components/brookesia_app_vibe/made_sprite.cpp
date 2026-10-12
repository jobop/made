// SPDX-License-Identifier: Apache-2.0
#include "made_sprite.hpp"
#include "vibe_theme.hpp"
#include "made_layout.hpp"

#include <algorithm>
#include <new>

LV_IMAGE_DECLARE(img_app_vibe);

namespace made_sprite {
namespace {

constexpr uint32_t kLime = 0xC7F58B;

struct Parts {
    lv_obj_t *glow = nullptr;      // 待确认/运行时的光圈（accent 色，脉冲）
    lv_obj_t *halo = nullptr;      // 舞台底盘
    lv_obj_t *body = nullptr;      // 圆形裁切的身体
    lv_obj_t *image = nullptr;     // 吉祥物图（后续换分层素材）
    lv_obj_t *shadow = nullptr;
    lv_obj_t *left_arm = nullptr;
    lv_obj_t *right_arm = nullptr;
    lv_obj_t *left_foot = nullptr;
    lv_obj_t *right_foot = nullptr;
    lv_obj_t *spark[3]{};
    lv_point_precise_t left_points[3]{};
    lv_point_precise_t right_points[3]{};
    int last_mood = -1;
    bool frozen = false;
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

lv_obj_t *create(lv_obj_t *parent) {
    auto *parts = new (std::nothrow) Parts;
    if (!parts) return nullptr;
    auto *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, made_s(0), made_s(0));
    lv_obj_set_size(root, made_screen_w(), made_screen_h());
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(root, parts);
    lv_obj_add_event_cb(root, destroy, LV_EVENT_DELETE, parts);

    const auto &palette = vibe_theme::palette();
    // 光圈：平时隐形，待确认/运行中点亮（update() 里驱动脉冲）。
    parts->glow = shape(root, 58, 40, 244, 244, palette.accent, LV_OPA_TRANSP);
    // 舞台底盘：比锁屏更占屏，居中略偏上，给底部名字和页码留空间。
    parts->halo = shape(root, 72, 54, 216, 216, 0x12282B);
    lv_obj_set_style_bg_grad_color(parts->halo, lv_color_hex(palette.bg_page), 0);
    lv_obj_set_style_bg_grad_dir(parts->halo, LV_GRAD_DIR_VER, 0);
    parts->shadow = shape(root, 116, 288, 128, 12, 0x5B846D, 55);
    parts->left_arm = line(root, 0xABD885, 7);
    parts->right_arm = line(root, 0xABD885, 7);
    parts->left_foot = shape(root, 138, 266, 22, 10, kLime);
    parts->right_foot = shape(root, 196, 266, 22, 10, kLime);

    // 圆形裁切放大豆豆图：锁屏 136px 舞台的约 1.6 倍。
    parts->body = shape(root, 100, 62, 160, 160, 0x102629);
    lv_obj_set_size(parts->body, 160, 160);  // 设计像素，矩形屏再经 transform 缩放
    lv_obj_set_style_transform_pivot_x(parts->body, 0, 0);
    lv_obj_set_style_transform_pivot_y(parts->body, 0, 0);
    lv_obj_set_style_transform_scale_x(parts->body, made_lv_scale(), 0);
    lv_obj_set_style_transform_scale_y(parts->body, made_lv_scale(), 0);
    lv_obj_set_style_clip_corner(parts->body, true, 0);
    parts->image = lv_image_create(parts->body);
    passive(parts->image);
    lv_image_set_src(parts->image, &img_app_vibe);
    lv_obj_set_pos(parts->image, -8, -8);  // 112px 图放大到 ~172px，居中进 160px 圆
    lv_image_set_pivot(parts->image, 56, 56);
    lv_image_set_scale(parts->image, 172 * LV_SCALE_NONE / 112);
    lv_image_set_antialias(parts->image, true);

    parts->spark[0] = shape(root, 64, 132, 5, 5, kLime, 160);
    parts->spark[1] = shape(root, 272, 108, 5, 5, kLime, 190);
    parts->spark[2] = shape(root, 248, 46, 4, 4, 0x87CEC1, 130);

    update(root, 0, Mood::kIdle);
    return root;
}

void update(lv_obj_t *root, uint32_t elapsed_ms, int mood) {
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || parts->body == nullptr) return;
    if (mood == Mood::kOffline) {
        if (parts->frozen) return;
        parts->frozen = true;  // 静止在一帧中间姿态
        elapsed_ms = 900;
    } else {
        parts->frozen = false;
    }

    uint32_t sway_period = 1800, bounce_period = 900, step_period = 1800;
    int sway_amp = 7, bounce_amp = 8, step_amp = 4, angle_amp = 45;
    if (mood == Mood::kTalking) {
        sway_period = 1200; bounce_period = 450; step_period = 1200;
        sway_amp = 9; bounce_amp = 10; step_amp = 6; angle_amp = 70;
    } else if (mood == Mood::kRunning) {
        sway_period = 1300; bounce_period = 600; step_period = 1300;
        sway_amp = 6; bounce_amp = 6; step_amp = 3; angle_amp = 30;
    } else if (mood == Mood::kWaitingConfirm) {
        sway_period = 2200; bounce_period = 700; step_period = 2200;
        sway_amp = 5; bounce_amp = 5; step_amp = 2; angle_amp = 20;
    }
    const int sway = wave(elapsed_ms, sway_period, sway_amp);
    const int bounce = (wave(elapsed_ms, bounce_period, bounce_amp, 270) + bounce_amp) / 2;
    const int step = wave(elapsed_ms, step_period, step_amp);
    lv_obj_set_pos(parts->body, made_x(100 + sway), made_y(62 - bounce));
    const int angle = wave(elapsed_ms, sway_period, angle_amp);
    lv_image_set_rotation(parts->image, angle < 0 ? angle + 3600 : angle);
    lv_obj_set_width(parts->shadow, made_s(128 - bounce * 2));
    lv_obj_set_x(parts->shadow, made_x(116 + bounce + sway / 2));
    lv_obj_set_style_bg_opa(parts->shadow, 55 - bounce * 2, 0);

    parts->left_points[0] = {made_x(120 + sway), made_y(186 - bounce)};
    parts->left_points[1] = {made_x(88 + sway), made_y(168 - bounce)};
    parts->left_points[2] = {made_x(74 + sway), made_y(132 - step * 2 - bounce)};
    parts->right_points[0] = {made_x(240 + sway), made_y(182 - bounce)};
    parts->right_points[1] = {made_x(268 + sway), made_y(166 - bounce)};
    parts->right_points[2] = {made_x(282 + sway), made_y(134 + step * 2 - bounce)};
    lv_line_set_points(parts->left_arm, parts->left_points, 3);
    lv_line_set_points(parts->right_arm, parts->right_points, 3);
    lv_obj_set_pos(parts->left_foot, made_x(138 + sway - step), made_y(266 - std::max(0, step) - bounce / 2));
    lv_obj_set_pos(parts->right_foot, made_x(196 + sway - step), made_y(266 - std::max(0, -step) - bounce / 2));
    for (int i = 0; i < 3; ++i)
        lv_obj_set_style_bg_opa(parts->spark[i], 135 + wave(elapsed_ms, 2100, 50, i * 100), 0);

    // 光圈语义：待确认 = accent 脉冲（提醒双击确认/三击取消）；运行中 = 微光常亮；其余隐形。
    if (mood == Mood::kWaitingConfirm) {
        const int glow_opa = 120 + wave(elapsed_ms, 1400, 80);
        lv_obj_set_style_bg_opa(parts->glow, static_cast<lv_opa_t>(glow_opa), 0);
    } else if (mood == Mood::kRunning) {
        lv_obj_set_style_bg_opa(parts->glow, static_cast<lv_opa_t>(45), 0);
    } else {
        lv_obj_set_style_bg_opa(parts->glow, LV_OPA_TRANSP, 0);
    }
    if (mood != parts->last_mood) {
        parts->last_mood = mood;
        lv_obj_invalidate(root);
    }
}

} // namespace made_sprite
