// SPDX-License-Identifier: Apache-2.0
#include "made_sprite.hpp"
#include "vibe_theme.hpp"
#include "made_layout.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <png.h>

LV_IMAGE_DECLARE(img_app_vibe);

namespace made_sprite {
namespace {

constexpr uint32_t kLime = 0xC7F58B;

struct Parts {
    lv_obj_t *glow = nullptr;      // 待确认/运行时的光圈（accent 色，脉冲）
    lv_obj_t *halo = nullptr;      // 舞台底盘
    lv_obj_t *body = nullptr;      // 圆形裁切的身体
    lv_obj_t *image = nullptr;     // 吉祥物图（后续换分层素材）
    lv_obj_t *lid_left = nullptr;  // 眼皮：眨眼时盖住眼睛（脸部同色）
    lv_obj_t *lid_right = nullptr;
    lv_obj_t *shadow = nullptr;
    lv_obj_t *left_arm = nullptr;
    lv_obj_t *right_arm = nullptr;
    lv_obj_t *left_foot = nullptr;
    lv_obj_t *right_foot = nullptr;
    lv_obj_t *spark[3]{};
    lv_point_precise_t left_points[3]{};
    lv_point_precise_t right_points[3]{};
    int image_base_scale = 256;    // 素材铺满身体圆的基础缩放（呼吸在其上波动）
    int last_mood = -1;
    bool frozen = false;

    // 分层素材模式（docs/SPRITES.md）。active 时占位形象隐藏。
    struct Frame {
        lv_image_dsc_t dsc{};
        uint8_t *pixels = nullptr;   // PSRAM；解码一次，永久被 dsc 引用
    };
    struct ArtLayer {
        lv_obj_t *obj = nullptr;
        std::vector<Frame> frames;
        int base_scale = 256;        // 设计像素 → 屏幕像素
        bool blink = false;
        bool talk = false;
        bool alert = false;
        uint32_t blink_period = 4000;
        uint32_t blink_closed = 120;
    };
    struct Art {
        lv_obj_t *container = nullptr;
        std::vector<ArtLayer> layers;
        uint32_t breath_period = 3000;
        int breath_pct = 103;
        uint32_t sway_period = 1800;
        int sway_amp = 7;
        int bounce_amp = 8;
        bool active = false;

        ~Art() {
            for (auto &layer : layers)
                for (auto &frame : layer.frames)
                    if (frame.pixels) heap_caps_free(frame.pixels);
        }
    } art;
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
    lv_obj_set_pos(object, made_x(x), made_home_y(y));
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

// PNG → ARGB8888（PNG_FORMAT_BGRA 的内存字节序与 LVGL ARGB8888 一致，同
// esp_lv_decoder 的做法）。缓冲驻留 PSRAM，只在应用素材时各调一次。
uint8_t *decodePngArgb(const std::string &path, int &width, int &height) {
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str())) {
        png_image_free(&image);
        return nullptr;
    }
    image.format = PNG_FORMAT_BGRA;
    const size_t bytes = static_cast<size_t>(image.width) * image.height * 4;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) {
        png_image_free(&image);
        return nullptr;
    }
    if (!png_image_finish_read(&image, nullptr, buffer, 0, nullptr)) {
        png_image_free(&image);
        heap_caps_free(buffer);
        return nullptr;
    }
    width = static_cast<int>(image.width);
    height = static_cast<int>(image.height);
    return buffer;
}

void setMascotHidden(Parts *parts, bool hidden) {
    auto apply = [hidden](lv_obj_t *object) {
        if (!object) return;
        if (hidden) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    };
    apply(parts->halo);
    apply(parts->body);
    apply(parts->shadow);
    apply(parts->left_arm);
    apply(parts->right_arm);
    apply(parts->left_foot);
    apply(parts->right_foot);
    for (auto *spark : parts->spark) apply(spark);
}

void destroy(lv_event_t *event) {
    auto *parts = static_cast<Parts *>(lv_event_get_user_data(event));
    lv_obj_set_user_data(lv_event_get_target_obj(event), nullptr);
    delete parts;
}

// 素材模式的动效驱动。呼吸/轻摆作用于容器与图层缩放；图层自身的
// blink/talk/alert 在这里按状态机切帧。LRU 素材（占位形象）参数不参与。
void artUpdate(Parts *parts, uint32_t elapsed_ms, int mood) {
    auto &art = parts->art;
    if (mood == Mood::kOffline) elapsed_ms = 900;  // 与占位形象一致：静止在中间帧
    const int sway = wave(elapsed_ms, art.sway_period, art.sway_amp);
    const int bounce = (wave(elapsed_ms, art.sway_period / 2, art.bounce_amp, 270) + art.bounce_amp) / 2;
    lv_obj_set_pos(art.container, made_x(sway), made_home_y(-bounce));
    const int breath_delta = wave(elapsed_ms, art.breath_period,
                                  (art.breath_pct - 100) * 256 / 100);
    for (auto &layer : art.layers) {
        if (!layer.obj || layer.frames.empty()) continue;
        int frame = 0;
        if (layer.blink) {
            frame = (elapsed_ms % layer.blink_period) < layer.blink_closed ? 1 : 0;
        } else if (layer.talk && (mood == Mood::kTalking || mood == Mood::kRunning)) {
            frame = static_cast<int>((elapsed_ms / 180) % layer.frames.size());
        }
        frame = std::min(std::max(frame, 0), static_cast<int>(layer.frames.size()) - 1);
        lv_image_set_src(layer.obj, &layer.frames[static_cast<size_t>(frame)].dsc);
        lv_image_set_scale(layer.obj, layer.base_scale + breath_delta);
        lv_obj_set_style_img_opa(layer.obj,
            layer.alert && mood == Mood::kWaitingConfirm
                ? static_cast<lv_opa_t>(140 + wave(elapsed_ms, 1400, 90)) : LV_OPA_COVER, 0);
    }
    // 光圈语义与占位形象一致。
    if (mood == Mood::kWaitingConfirm)
        lv_obj_set_style_bg_opa(parts->glow, static_cast<lv_opa_t>(120 + wave(elapsed_ms, 1400, 80)), 0);
    else if (mood == Mood::kRunning)
        lv_obj_set_style_bg_opa(parts->glow, static_cast<lv_opa_t>(45), 0);
    else
        lv_obj_set_style_bg_opa(parts->glow, LV_OPA_TRANSP, 0);
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

    // 圆形裁切放大豆豆图：136px 锁屏舞台的约 1.6 倍。图片与身体都用屏幕缩放，
    // 矩形屏上不再裁脸（此前图未乘屏幕缩放，长屏只剩一张放大的脸）。
    parts->body = shape(root, 100, 62, 160, 160, 0x102629);
    lv_obj_set_style_clip_corner(parts->body, true, 0);
    parts->image = lv_image_create(parts->body);
    passive(parts->image);
    lv_image_set_src(parts->image, &img_app_vibe);
    const int show = made_s(172);  // 112px 素材铺到 172 设计像素，超出圆被裁掉
    parts->image_base_scale = show * LV_SCALE_NONE / 112;
    lv_image_set_scale(parts->image, parts->image_base_scale);
    lv_obj_set_pos(parts->image, (made_s(160) - show) / 2, (made_s(160) - show) / 2);
    lv_image_set_pivot(parts->image, 56, 56);
    lv_image_set_antialias(parts->image, true);

    // 眼皮：脸部同色圆盘，平时透明，眨眼时短暂盖住眼睛。眼睛中心从素材网格图
    // 实测：112px 图上左眼 (39,50)、右眼 (68,46)；图片以 172 设计像素铺入
    // 160 圆（原点 -6、系数 1.536）→ 设计坐标约 (54,71) / (98,65)。
    constexpr uint32_t kFace = 0xE2F57F;
    auto makeLid = [&parts](int eye_x, int eye_y) {
        lv_obj_t *lid = lv_obj_create(parts->body);
        lv_obj_remove_style_all(lid);
        passive(lid);
        lv_obj_set_size(lid, made_s(34), made_s(34));
        lv_obj_set_pos(lid, made_s(eye_x - 17), made_s(eye_y - 17));
        lv_obj_set_style_radius(lid, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lid, lv_color_hex(kFace), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_TRANSP, 0);
        return lid;
    };
    parts->lid_left = makeLid(54, 71);
    parts->lid_right = makeLid(98, 65);

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
    if (parts->art.active) {
        artUpdate(parts, elapsed_ms, mood);
        if (mood != parts->last_mood) {
            parts->last_mood = mood;
            lv_obj_invalidate(root);
        }
        return;
    }
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
    lv_obj_set_pos(parts->body, made_x(100 + sway), made_home_y(62 - bounce));
    const int angle = wave(elapsed_ms, sway_period, angle_amp);
    lv_image_set_rotation(parts->image, angle < 0 ? angle + 3600 : angle);
    // 呼吸：整体轻微缩放（±1.6%），与蹦跳的相位错开，待机时也能看出「活着」。
    const int breath = wave(elapsed_ms, 3200, 4);
    lv_image_set_scale(parts->image, parts->image_base_scale + breath);
    // 眨眼：每 ~4.2s 闭一次（约 130ms）；离线冻结时不眨。
    const bool blink = mood != Mood::kOffline && (elapsed_ms % 4200) < 130;
    lv_obj_set_style_bg_opa(parts->lid_left, blink ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(parts->lid_right, blink ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_width(parts->shadow, made_s(128 - bounce * 2));
    lv_obj_set_x(parts->shadow, made_x(116 + bounce + sway / 2));
    lv_obj_set_style_bg_opa(parts->shadow, 55 - bounce * 2, 0);

    parts->left_points[0] = {made_x(120 + sway), made_home_y(186 - bounce)};
    parts->left_points[1] = {made_x(88 + sway), made_home_y(168 - bounce)};
    parts->left_points[2] = {made_x(74 + sway), made_home_y(132 - step * 2 - bounce)};
    parts->right_points[0] = {made_x(240 + sway), made_home_y(182 - bounce)};
    parts->right_points[1] = {made_x(268 + sway), made_home_y(166 - bounce)};
    parts->right_points[2] = {made_x(282 + sway), made_home_y(134 + step * 2 - bounce)};
    lv_line_set_points(parts->left_arm, parts->left_points, 3);
    lv_line_set_points(parts->right_arm, parts->right_points, 3);
    lv_obj_set_pos(parts->left_foot, made_x(138 + sway - step), made_home_y(266 - std::max(0, step) - bounce / 2));
    lv_obj_set_pos(parts->right_foot, made_x(196 + sway - step), made_home_y(266 - std::max(0, -step) - bounce / 2));
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

bool applyArtwork(lv_obj_t *root, const std::string &manifest_json, const std::string &base_dir) {
    if (!root) return false;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->body) return false;
    cJSON *manifest = cJSON_Parse(manifest_json.c_str());
    if (!manifest) return false;
    const cJSON *canvas = cJSON_GetObjectItemCaseSensitive(manifest, "canvas");
    const cJSON *canvas_w = cJSON_GetObjectItemCaseSensitive(canvas, "w");
    const cJSON *canvas_h = cJSON_GetObjectItemCaseSensitive(canvas, "h");
    const cJSON *layers = cJSON_GetObjectItemCaseSensitive(manifest, "layers");
    if (!cJSON_IsNumber(canvas_w) || !cJSON_IsNumber(canvas_h) || !cJSON_IsArray(layers) ||
        canvas_w->valueint <= 0 || canvas_h->valueint <= 0) {
        cJSON_Delete(manifest);
        return false;
    }

    // 按 z 排序后逐层建对象；全部成功才提交，中途失败整体回滚。
    struct Pending {
        const cJSON *json;
        int z;
    };
    std::vector<Pending> pending;
    for (const cJSON *layer = layers->child; layer; layer = layer->next) {
        if (!cJSON_IsObject(layer) || pending.size() >= 16) continue;
        const cJSON *z = cJSON_GetObjectItemCaseSensitive(layer, "z");
        pending.push_back({layer, cJSON_IsNumber(z) ? z->valueint : 0});
    }
    std::stable_sort(pending.begin(), pending.end(),
                     [](const Pending &a, const Pending &b) { return a.z < b.z; });

    Parts::Art next;
    auto *container = lv_obj_create(root);
    lv_obj_remove_style_all(container);
    passive(container);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, made_screen_w(), made_screen_h());

    // 整体动效参数：manifest 缺省时用固件默认值（呼吸 3% / 3000ms，摆 7px / 1800ms）。
    const cJSON *motion = cJSON_GetObjectItemCaseSensitive(manifest, "motion");
    if (cJSON_IsObject(motion)) {
        const cJSON *breath = cJSON_GetObjectItemCaseSensitive(motion, "breath");
        if (cJSON_IsObject(breath)) {
            const cJSON *pct = cJSON_GetObjectItemCaseSensitive(breath, "scale_pct");
            const cJSON *period = cJSON_GetObjectItemCaseSensitive(breath, "period_ms");
            if (cJSON_IsNumber(pct) && pct->valueint >= 100 && pct->valueint <= 120)
                next.breath_pct = pct->valueint;
            if (cJSON_IsNumber(period) && period->valueint >= 500 && period->valueint <= 10000)
                next.breath_period = static_cast<uint32_t>(period->valueint);
        }
        const cJSON *sway_deg = cJSON_GetObjectItemCaseSensitive(motion, "sway_deg");
        if (cJSON_IsNumber(sway_deg) && sway_deg->valueint >= 0 && sway_deg->valueint <= 15)
            next.sway_amp = 2 + sway_deg->valueint;
        const cJSON *sway_period = cJSON_GetObjectItemCaseSensitive(motion, "sway_period_ms");
        if (cJSON_IsNumber(sway_period) && sway_period->valueint >= 200 && sway_period->valueint <= 10000)
            next.sway_period = static_cast<uint32_t>(sway_period->valueint);
    }

    for (const auto &entry : pending) {
        const cJSON *layer = entry.json;
        std::vector<std::string> files;
        const cJSON *frames = cJSON_GetObjectItemCaseSensitive(layer, "frames");
        if (cJSON_IsArray(frames)) {
            for (const cJSON *frame = frames->child; frame; frame = frame->next)
                if (cJSON_IsString(frame) && frame->valuestring) files.push_back(frame->valuestring);
        } else {
            const cJSON *file = cJSON_GetObjectItemCaseSensitive(layer, "file");
            if (cJSON_IsString(file) && file->valuestring) files.push_back(file->valuestring);
        }
        Parts::ArtLayer art_layer;
        const cJSON *blink = cJSON_GetObjectItemCaseSensitive(layer, "blink");
        if (cJSON_IsObject(blink)) {
            const cJSON *period = cJSON_GetObjectItemCaseSensitive(blink, "period_ms");
            const cJSON *closed = cJSON_GetObjectItemCaseSensitive(blink, "closed_ms");
            if (cJSON_IsNumber(period)) art_layer.blink_period = static_cast<uint32_t>(period->valueint);
            if (cJSON_IsNumber(closed)) art_layer.blink_closed = static_cast<uint32_t>(closed->valueint);
            art_layer.blink = true;
        }
        art_layer.talk = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(layer, "talk"));
        const cJSON *alert = cJSON_GetObjectItemCaseSensitive(layer, "alert");
        art_layer.alert = cJSON_IsString(alert) && std::strcmp(alert->valuestring, "pulse") == 0;

        const cJSON *x = cJSON_GetObjectItemCaseSensitive(layer, "x");
        const cJSON *y = cJSON_GetObjectItemCaseSensitive(layer, "y");
        for (const auto &name : files) {
            // 文件名白名单与桥接器校验一致；路径拼接后仍限制在 base_dir 内。
            if (name.empty() || name.size() > 32 ||
                name.find_first_of("/\\") != std::string::npos ||
                name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos) {
                goto fail;
            }
            int width = 0, height = 0;
            auto *pixels = decodePngArgb(base_dir + name, width, height);
            if (!pixels) goto fail;
            Parts::Frame frame;
            frame.pixels = pixels;
            frame.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            frame.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
            frame.dsc.header.w = static_cast<uint32_t>(width);
            frame.dsc.header.h = static_cast<uint32_t>(height);
            frame.dsc.header.stride = static_cast<uint32_t>(width) * 4;
            frame.dsc.data_size = static_cast<size_t>(width) * height * 4;
            frame.dsc.data = pixels;
            art_layer.frames.push_back(frame);
        }
        if (art_layer.frames.empty() || !cJSON_IsNumber(x) || !cJSON_IsNumber(y)) goto fail;

        {
            auto *object = lv_image_create(container);
            passive(object);
            lv_image_set_src(object, &art_layer.frames.front().dsc);
            lv_obj_set_pos(object, made_x(x->valueint), made_home_y(y->valueint));
            art_layer.base_scale = made_lv_scale();
            lv_image_set_scale(object, art_layer.base_scale);
            lv_image_set_antialias(object, true);
            // 缩放支点对齐画布中心：所有层围绕同一点呼吸。
            lv_image_set_pivot(object,
                std::clamp(canvas_w->valueint / 2 - x->valueint, 0,
                           static_cast<int>(art_layer.frames.front().dsc.header.w)),
                std::clamp(canvas_h->valueint / 2 - y->valueint, 0,
                           static_cast<int>(art_layer.frames.front().dsc.header.h)));
            art_layer.obj = object;
        }
        next.layers.push_back(std::move(art_layer));
    }
    if (next.layers.empty()) goto fail;

    // 提交：删旧容器（占位形象恢复可见性被隐藏覆盖），换上新素材。
    if (parts->art.container) lv_obj_delete(parts->art.container);
    parts->art.container = next.container;
    parts->art.layers = std::move(next.layers);
    parts->art.breath_period = next.breath_period;
    parts->art.breath_pct = next.breath_pct;
    parts->art.sway_period = next.sway_period;
    parts->art.sway_amp = next.sway_amp;
    parts->art.bounce_amp = next.bounce_amp;
    parts->art.active = true;
    setMascotHidden(parts, true);
    cJSON_Delete(manifest);
    lv_obj_invalidate(root);
    return true;

fail:
    lv_obj_delete(container);
    cJSON_Delete(manifest);
    return false;
}

void clearArtwork(lv_obj_t *root) {
    if (!root) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->art.active) return;
    if (parts->art.container) lv_obj_delete(parts->art.container);
    parts->art.container = nullptr;
    parts->art.layers.clear();
    parts->art.active = false;
    setMascotHidden(parts, false);
    lv_obj_invalidate(root);
}

bool artworkActive(lv_obj_t *root) {
    if (!root) return false;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    return parts && parts->art.active;
}

} // namespace made_sprite
