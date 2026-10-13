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
#include <esp_log.h>
#include <png.h>

#include "assets/img_sprite_bean.h"

// 内置绿豆不是特例：它与第三方助手的素材（docs/SPRITES.md）走同一条分层
// 管线，只是素材编在固件里（img_sprite_bean.c）而不是从桥接器拉取。
// 固件里没有任何写死的五官坐标——眨眼/说话都是特征层切帧，眼位来自
// manifest 的 x/y（内置豆的坐标由 split-bean-layers.py 生成到 img_sprite_bean.h）。

LV_IMG_DECLARE(img_bean_body);
LV_IMG_DECLARE(img_bean_eyes_open);
LV_IMG_DECLARE(img_bean_eyes_closed);
LV_IMG_DECLARE(img_bean_mouth_closed);
LV_IMG_DECLARE(img_bean_mouth_open);

namespace made_sprite {
namespace {

struct Frame {
    lv_image_dsc_t dsc{};
    uint8_t *pixels = nullptr;   // PSRAM 堆分配（下发素材）；内置素材为 null（静态）
};

struct ArtLayer {
    lv_obj_t *obj = nullptr;
    std::vector<Frame> frames;
    int base_scale = 256;        // 设计像素 → 屏幕像素
    int base_x = 0;              // 未呼吸缩放时的屏幕位置（呼吸补偿的基准）
    int base_y = 0;
    bool blink = false;
    bool talk = false;
    bool alert = false;
    uint32_t blink_period = 4000;
    uint32_t blink_closed = 120;
    int last_frame = -1;
};

struct Parts {
    lv_obj_t *root = nullptr;      // 舞台根对象
    lv_obj_t *glow = nullptr;      // 待确认/运行时的光圈（accent 色，脉冲）
    bool builtin = true;           // 当前显示内置绿豆（setTint 只对它生效）
    int last_mood = -1;
    struct Art {
        lv_obj_t *container = nullptr;
        std::vector<ArtLayer> layers;
        uint32_t breath_period = 3000;
        int breath_pct = 108;
        uint32_t sway_period = 1800;
        int sway_amp = 7;
        int bounce_amp = 8;
        int pivot_x = 0;             // 呼吸支点（屏幕像素，画布中心）
        int pivot_y = 0;
        bool active = false;

        ~Art() {
            for (auto &layer : layers)
                for (auto &frame : layer.frames)
                    if (frame.pixels) heap_caps_free(frame.pixels);
        }
    } art;
};

void freeFrames(std::vector<Frame> &frames) {
    for (auto &frame : frames)
        if (frame.pixels) heap_caps_free(frame.pixels);
    frames.clear();
}

// 一个待提交的图层：坐标是 360 设计画布像素，frames 的像素所有权随 vector 移交。
struct LayerSpec {
    int x = 0;
    int y = 0;
    int scale_mul = 1;           // 素材基准像素相对设计像素的倍数（内置豆 = 3）
    bool blink = false;
    bool talk = false;
    bool alert = false;
    uint32_t blink_period = 4000;
    uint32_t blink_closed = 120;
    std::vector<Frame> frames;
};

struct MotionSpec {
    int canvas_w = made_design;
    int canvas_h = made_design;
    uint32_t breath_period = 3000;
    int breath_pct = 108;
    uint32_t sway_period = 1800;
    int sway_amp = 7;
    int bounce_amp = 8;
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

// 把规格提交成 LVGL 对象树；任一对象创建失败则整体回滚并保留旧素材。
bool commitArt(Parts *parts, const MotionSpec &motion, std::vector<LayerSpec> &&specs, bool builtin) {
    if (specs.empty()) return false;
    auto *container = lv_obj_create(parts->root);
    if (!container) return false;
    lv_obj_remove_style_all(container);
    passive(container);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, made_screen_w(), made_screen_h());

    // 先在本地把整套层建好：中途失败时旧素材可以原样留着，不破坏在线状态。
    std::vector<ArtLayer> layers;
    for (auto &spec : specs) {
        if (spec.frames.empty()) goto fail;
        auto *object = lv_image_create(container);
        if (!object) goto fail;
        passive(object);
        lv_image_set_src(object, &spec.frames.front().dsc);
        lv_obj_set_pos(object, made_x(spec.x), made_home_y(spec.y));
        const int base_scale = spec.scale_mul * made_lv_scale();
        lv_image_set_scale(object, base_scale);
        // 缩放支点必须显式设为左上角：lv_image 的 pivot 默认是图片中心，
        // 不设的话每层绕各自中心放大，层间相对位置全部漂移（实机右下角
        // 的「绿色色块」就是被甩出去的嘴层/豆脚）。
        lv_image_set_pivot(object, 0, 0);
        lv_image_set_antialias(object, true);
        // 呼吸缩放用逐层 lv_image_set_scale + 位置补偿（见 artUpdate）：
        // 容器级 style transform 的变换域是整屏对象，每帧重采样整个屏幕，
        // 实测是动效卡顿的主源；逐层图像变换只覆盖图层自身 bbox，且能走
        // PPA 加速的单图变换路径。
        ArtLayer layer;
        layer.obj = object;
        layer.frames = std::move(spec.frames);
        layer.base_scale = base_scale;
        layer.base_x = made_x(spec.x);
        layer.base_y = made_home_y(spec.y);
        layer.blink = spec.blink;
        layer.talk = spec.talk;
        layer.alert = spec.alert;
        layer.blink_period = spec.blink_period;
        layer.blink_closed = spec.blink_closed;
        layers.push_back(std::move(layer));
    }

    // 提交：旧素材（含内置豆）整体退场。层表和容器必须一起换——旧层的 obj 指向
    // 即将删除的旧容器，留在表里 artUpdate 就会操作已释放对象（实机切素材那一帧
    // LoadProhibited 崩溃，随后重启把助手选择也带回 0 号）。
    if (parts->art.container) lv_obj_delete(parts->art.container);
    for (auto &layer : parts->art.layers) freeFrames(layer.frames);
    parts->art.layers = std::move(layers);
    parts->art.container = container;
    parts->art.breath_period = motion.breath_period;
    parts->art.breath_pct = motion.breath_pct;
    parts->art.sway_period = motion.sway_period;
    parts->art.sway_amp = motion.sway_amp;
    parts->art.bounce_amp = motion.bounce_amp;
    parts->art.pivot_x = made_x(motion.canvas_w / 2);
    parts->art.pivot_y = made_home_y(motion.canvas_h / 2);
    parts->art.active = true;
    parts->builtin = builtin;
    for (const auto &layer : parts->art.layers) {
        ESP_LOGI("made_sprite", "[art] layer frames=%u blink=%d period=%u closed=%u",
            static_cast<unsigned>(layer.frames.size()), layer.blink ? 1 : 0,
            static_cast<unsigned>(layer.blink_period), static_cast<unsigned>(layer.blink_closed));
    }
    lv_obj_invalidate(parts->root);
    return true;

fail:
    // 只回收本轮已建成的层：旧素材与旧层表保持可用，等下一次提交再换。
    for (auto &layer : layers) freeFrames(layer.frames);
    lv_obj_delete(container);
    return false;
}

void destroy(lv_event_t *event) {
    auto *parts = static_cast<Parts *>(lv_event_get_user_data(event));
    lv_obj_set_user_data(lv_event_get_target_obj(event), nullptr);
    delete parts;
}

// 内置绿豆：一份用 SPRITES 格式表达的普通素材（分层 + manifest 语义），
// 与桥接器下发的助手素材走完全相同的提交与驱动路径。
bool applyBeanArt(Parts *parts) {
    // 特征层坐标是 112 基准的全画布坐标；素材铺到 336 设计像素（×3）。
    constexpr int kMul = 3;
    const int body_w = img_bean_body.header.w * kMul;
    const int body_h = img_bean_body.header.h * kMul;
    const int origin_x = (made_design - body_w) / 2;
    const int origin_y = (made_design - body_h) / 2;

    std::vector<LayerSpec> specs;
    LayerSpec body;
    body.x = origin_x;
    body.y = origin_y;
    body.scale_mul = kMul;
    body.frames.push_back(Frame{});
    body.frames.back().dsc = img_bean_body;
    specs.push_back(std::move(body));

    LayerSpec eyes;
    eyes.x = origin_x + IMG_BEAN_EYES_X * kMul;
    eyes.y = origin_y + IMG_BEAN_EYES_Y * kMul;
    eyes.scale_mul = kMul;
    eyes.blink = true;
    eyes.blink_period = 4200;
    eyes.blink_closed = 130;
    eyes.frames.push_back(Frame{});
    eyes.frames.back().dsc = img_bean_eyes_open;
    eyes.frames.push_back(Frame{});
    eyes.frames.back().dsc = img_bean_eyes_closed;
    specs.push_back(std::move(eyes));

    LayerSpec mouth;
    mouth.x = origin_x + IMG_BEAN_MOUTH_X * kMul;
    mouth.y = origin_y + IMG_BEAN_MOUTH_Y * kMul;
    mouth.scale_mul = kMul;
    mouth.talk = true;
    mouth.frames.push_back(Frame{});
    mouth.frames.back().dsc = img_bean_mouth_closed;
    mouth.frames.push_back(Frame{});
    mouth.frames.back().dsc = img_bean_mouth_open;
    specs.push_back(std::move(mouth));

    MotionSpec motion;  // 缺省动效：呼吸 8% / 3000ms，摆 7px / 1800ms
    return commitArt(parts, motion, std::move(specs), true);
}

// 素材模式的动效驱动。呼吸/轻摆作用于容器（位置 + 单次缩放变换，支点在
// 舞台中心）；blink/talk/alert 层在这里按状态机切帧。节奏随情绪微调。
void artUpdate(Parts *parts, uint32_t elapsed_ms, int mood) {
    auto &art = parts->art;
    if (mood == Mood::kOffline) elapsed_ms = 900;  // 静止在中间帧
    uint32_t sway_period = art.sway_period;
    int sway_amp = art.sway_amp;
    int bounce_amp = art.bounce_amp;
    if (mood == Mood::kTalking) {
        sway_period = sway_period * 2 / 3;
        sway_amp += 2;
        bounce_amp += 2;
    } else if (mood == Mood::kRunning) {
        sway_period = sway_period * 3 / 4;
    } else if (mood == Mood::kWaitingConfirm) {
        sway_period = sway_period * 6 / 5;
        sway_amp -= 2;
    }
    const int sway = wave(elapsed_ms, sway_period, sway_amp);
    const int bounce = (wave(elapsed_ms, sway_period / 2, bounce_amp, 270) + bounce_amp) / 2;
    lv_obj_set_pos(art.container, made_x(sway), made_home_y(-bounce));
    // 呼吸：逐层图像缩放 + 位置补偿，支点=画布中心。缩放比 s 时层的位置
    // 必须从 base_pos 变为 center + (base_pos - center) * s —— 等价于容器级
    // 变换的几何效果，但变换域从整屏缩小到各图层自身 bbox，也不再走整屏
    // 重采样（此前动效一卡一卡的主源）。此前 bisect 排查层间漂移时临时
    // 关过呼吸，根因是逐层 pivot 与层表残留（均已修），恢复之。
    const int amp = (art.breath_pct - 100) * 256 / 200;
    const int scale = 256 + wave(elapsed_ms, art.breath_period, amp);
    for (auto &layer : art.layers) {
        if (!layer.obj) continue;
        if (scale != 256) {
            lv_obj_set_pos(layer.obj,
                art.pivot_x + (layer.base_x - art.pivot_x) * scale / 256,
                art.pivot_y + (layer.base_y - art.pivot_y) * scale / 256);
            lv_image_set_scale(layer.obj, layer.base_scale * scale / 256);
        } else {
            // 100% 基准：值与上一 tick 相同时 LVGL 不会重复失效
            lv_obj_set_pos(layer.obj, layer.base_x, layer.base_y);
            lv_image_set_scale(layer.obj, layer.base_scale);
        }
    }
    for (auto &layer : art.layers) {
        if (!layer.obj || layer.frames.empty()) continue;
        int frame = 0;
        if (layer.blink) {
            frame = (elapsed_ms % layer.blink_period) < layer.blink_closed ? 1 : 0;
        } else if (layer.talk && (mood == Mood::kTalking || mood == Mood::kRunning)) {
            frame = static_cast<int>((elapsed_ms / 180) % layer.frames.size());
        }
        frame = std::min(std::max(frame, 0), static_cast<int>(layer.frames.size()) - 1);
        if (frame != layer.last_frame) {
            layer.last_frame = frame;
            ESP_LOGI("made_sprite", "[frame] blink=%d talk=%d mood=%d frame=%d elapsed=%u",
                layer.blink ? 1 : 0, layer.talk ? 1 : 0, mood, frame,
                static_cast<unsigned>(elapsed_ms));
        }
        lv_image_set_src(layer.obj, &layer.frames[static_cast<size_t>(frame)].dsc);
        lv_obj_set_style_img_opa(layer.obj,
            layer.alert && mood == Mood::kWaitingConfirm
                ? static_cast<lv_opa_t>(140 + wave(elapsed_ms, 1400, 90)) : LV_OPA_COVER, 0);
    }
    // 光圈语义：待确认 = accent 脉冲（提醒双击确认/三击取消）；运行中 = 微光常亮。
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
    if (!root) {
        delete parts;
        return nullptr;
    }
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, made_s(0), made_s(0));
    lv_obj_set_size(root, made_screen_w(), made_screen_h());
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(root, parts);
    lv_obj_add_event_cb(root, destroy, LV_EVENT_DELETE, parts);
    parts->root = root;

    const auto &palette = vibe_theme::palette();
    // 光圈：平时隐形，待确认/运行中点亮（update() 里驱动脉冲）。
    // 满屏精灵身后的一整圈光环。
    parts->glow = shape(root, 18, 4, 324, 324, palette.accent, LV_OPA_TRANSP);

    applyBeanArt(parts);
    return root;
}

void update(lv_obj_t *root, uint32_t elapsed_ms, int mood) {
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->art.active) return;
    artUpdate(parts, elapsed_ms, mood);
    if (mood != parts->last_mood) {
        parts->last_mood = mood;
        lv_obj_invalidate(root);
    }
}

bool applyArtwork(lv_obj_t *root, const std::string &manifest_json, const std::string &base_dir) {
    if (!root) return false;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->root) return false;
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

    MotionSpec motion;
    motion.canvas_w = canvas_w->valueint;
    motion.canvas_h = canvas_h->valueint;
    const cJSON *motion_json = cJSON_GetObjectItemCaseSensitive(manifest, "motion");
    if (cJSON_IsObject(motion_json)) {
        const cJSON *breath = cJSON_GetObjectItemCaseSensitive(motion_json, "breath");
        if (cJSON_IsObject(breath)) {
            const cJSON *pct = cJSON_GetObjectItemCaseSensitive(breath, "scale_pct");
            const cJSON *period = cJSON_GetObjectItemCaseSensitive(breath, "period_ms");
            if (cJSON_IsNumber(pct) && pct->valueint >= 100 && pct->valueint <= 120)
                motion.breath_pct = pct->valueint;
            if (cJSON_IsNumber(period) && period->valueint >= 500 && period->valueint <= 10000)
                motion.breath_period = static_cast<uint32_t>(period->valueint);
        }
        const cJSON *sway_deg = cJSON_GetObjectItemCaseSensitive(motion_json, "sway_deg");
        if (cJSON_IsNumber(sway_deg) && sway_deg->valueint >= 0 && sway_deg->valueint <= 15)
            motion.sway_amp = 2 + sway_deg->valueint;
        const cJSON *sway_period = cJSON_GetObjectItemCaseSensitive(motion_json, "sway_period_ms");
        if (cJSON_IsNumber(sway_period) && sway_period->valueint >= 200 && sway_period->valueint <= 10000)
            motion.sway_period = static_cast<uint32_t>(sway_period->valueint);
    }

    // 按 z 排序后逐层建规格；素材在 LVGL 线程解码（数百毫秒级，一次性）。
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

    std::vector<LayerSpec> specs;
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
        LayerSpec spec;
        const cJSON *blink = cJSON_GetObjectItemCaseSensitive(layer, "blink");
        if (cJSON_IsObject(blink)) {
            const cJSON *period = cJSON_GetObjectItemCaseSensitive(blink, "period_ms");
            const cJSON *closed = cJSON_GetObjectItemCaseSensitive(blink, "closed_ms");
            if (cJSON_IsNumber(period)) spec.blink_period = static_cast<uint32_t>(period->valueint);
            if (cJSON_IsNumber(closed)) spec.blink_closed = static_cast<uint32_t>(closed->valueint);
            spec.blink = true;
        }
        spec.talk = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(layer, "talk"));
        const cJSON *alert = cJSON_GetObjectItemCaseSensitive(layer, "alert");
        spec.alert = cJSON_IsString(alert) && std::strcmp(alert->valuestring, "pulse") == 0;

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
            Frame frame;
            frame.pixels = pixels;
            frame.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            frame.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
            frame.dsc.header.w = static_cast<uint32_t>(width);
            frame.dsc.header.h = static_cast<uint32_t>(height);
            frame.dsc.header.stride = static_cast<uint32_t>(width) * 4;
            frame.dsc.data_size = static_cast<size_t>(width) * height * 4;
            frame.dsc.data = pixels;
            spec.frames.push_back(frame);
        }
        if (spec.frames.empty() || !cJSON_IsNumber(x) || !cJSON_IsNumber(y)) goto fail;
        spec.x = x->valueint;
        spec.y = y->valueint;
        specs.push_back(std::move(spec));
    }

    if (commitArt(parts, motion, std::move(specs), false)) {
        cJSON_Delete(manifest);
        lv_obj_invalidate(root);
        return true;
    }
fail:
    // commitArt 半途失败时已回滚 own frames；这里只需清理未提交的规格。
    for (auto &spec : specs) freeFrames(spec.frames);
    cJSON_Delete(manifest);
    return false;
}

void clearArtwork(lv_obj_t *root) {
    if (!root) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->art.active || parts->builtin) return;
    if (parts->art.container) lv_obj_delete(parts->art.container);
    parts->art.container = nullptr;
    for (auto &layer : parts->art.layers) freeFrames(layer.frames);
    parts->art.layers.clear();
    parts->art.active = false;
    applyBeanArt(parts);        // 回到内置绿豆（同一条管线）
}

bool artworkActive(lv_obj_t *root) {
    if (!root) return false;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    return parts && parts->art.active && !parts->builtin;
}

void setTint(lv_obj_t *root, uint32_t color) {
    if (!root) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->builtin || !parts->art.active) return;
    for (auto &layer : parts->art.layers) {
        if (!layer.obj) continue;
        if (color == 0) {
            lv_obj_set_style_img_recolor_opa(layer.obj, LV_OPA_TRANSP, 0);
        } else {
            lv_obj_set_style_img_recolor(layer.obj, lv_color_hex(color), 0);
            lv_obj_set_style_img_recolor_opa(layer.obj, LV_OPA_60, 0);
        }
    }
    lv_obj_invalidate(root);
}

} // namespace made_sprite
