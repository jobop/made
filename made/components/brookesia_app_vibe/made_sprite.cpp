// SPDX-License-Identifier: Apache-2.0
#include "made_sprite.hpp"
#include "made_gif.hpp"
#include "vibe_theme.hpp"
#include "made_layout.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <misc/cache/instance/lv_image_cache.h>
#include <esp_log.h>
#include <png.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 内置绿豆已按产品要求移除：素材缺失时舞台留空（只显示助手名与页码），
// 不再回退到绿豆占位。固件里没有任何写死的五官坐标——眨眼/说话都是
// 特征层切帧，眼位来自 manifest 的 x/y。

namespace made_sprite {
namespace {

struct Frame {
    lv_image_dsc_t dsc{};
    uint8_t *pixels = nullptr;   // PSRAM 堆分配（下发素材）；内置素材为 null（静态）
};

// 情绪表的帧选择：frame >= 0 表示该情绪下固定显示这一帧；cycle 表示按
// talk 的节奏循环全部帧。默认 {-1,false} = 回退到无情绪表的旧行为。
struct EmotionSel {
    int8_t frame = -1;
    bool cycle = false;
};

struct ArtLayer {
    lv_obj_t *obj = nullptr;
    std::vector<Frame> frames;
    std::string id;              // 层标识（emotions 按它引用层）
    bool has_emotions = false;   // manifest 里本层被 emotions 引用过
    std::vector<EmotionSel> mood_sel;   // kMoodCount 项，按下标取
    int base_scale = 256;        // 设计像素 → 屏幕像素
    int base_x = 0;              // 未呼吸缩放时的屏幕位置（呼吸补偿的基准）
    int base_y = 0;
    bool blink = false;
    bool talk = false;
    bool alert = false;
    uint32_t blink_period = 4000;
    uint32_t blink_closed = 120;
    int last_frame = -1;
    // —— 呼吸帧轮播（做法A）：body 层的呼吸缩放预烘焙成 K 帧显示尺寸位图，
    // 运行时逐拍只换 src + 位移，不再对整张大图做软件重采样。烘焙在后台任务
    // 进行（每次进出屏幕都会重建舞台，同步烘会把 UI 线程卡住约 2 秒），
    // 烘焙期间用动态缩放兜底显示；失败不再重试（bake_tried），回退动态缩放。
    bool baked = false;
    bool bake_tried = false;
    int bbox_x = 0;              // 内容 bbox 在源图中的偏移（源像素）
    int bbox_y = 0;
    std::vector<Frame> baked_frames;   // 帧 k 的缩放 = baked_scale[k]（256 基准）
    std::vector<int> baked_scale;
};

// —— 行动系统 ——
// manifest 的 "actions" 表，两种形态：
//   帧表动作 = 对既有层的帧序覆盖（tracks: 层id → 帧序号序列），素材层就是
//              动作素材（挥手 = arm 层多帧），没有新文件引用；
//   GIF 动作 = 整帧动画文件（gif + ms），播放期起一个全画布覆盖层流式解码。
struct ActionSpec {
    std::string name;
    bool gif = false;           // true = GIF 动作（tracks 无效）
    std::string gif_file;       // gif 动作的文件名（≤20 字符，白名单校验）
    uint32_t gif_ms = 3000;     // gif 动作播放时长（100–10000ms）
    uint32_t frame_ms = 120;    // 每步时长（30–2000ms，边界外回默认）
    int loops = 1;              // 播放遍数（1–10）
    uint32_t total_steps = 0;   // commit 时算好：最长轨道长度 × loops（gif 恒 0）
    struct Track {
        std::string layer;      // 层 id（commit 时解析成下标，解析失败整条丢弃）
        size_t index = 0;
        std::vector<int> frames;   // 帧序号序列（循环取用；越界帧在 commit 时修剪）
    };
    std::vector<Track> tracks;
};

struct Parts {
    lv_obj_t *root = nullptr;      // 舞台根对象
    lv_obj_t *glow = nullptr;      // 待确认/运行时的光圈（accent 色，脉冲）
    bool builtin = false;          // 兼容保留：内置素材已删除，恒为 false
    bool bake_enabled = true;      // 侧舞台关烘焙：省 PSRAM 给预取解码
    int last_mood = -1;
    uint32_t last_anim_ms = 0;     // 上次动效驱动的 elapsed（10Hz 节流）
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
        uint64_t epoch = 0;          // 每次 commitArt 递增；后台烘焙结果按它对账
        std::vector<ActionSpec> actions;   // 行动系统：素材声明的具名动作
        int active_action = -1;      // 正在播放的动作下标，-1 = 无
        uint32_t action_started_ms = 0;    // lv_tick_get() 起点
        std::vector<uint8_t> action_layers; // 被任意动作引用的层（禁止烘焙）
        std::string base_dir;        // 素材文件目录（GIF 动作按名读缓存文件）
        // —— GIF 动作播放状态（同一时刻最多一份）——
        made_gif::Decoder gif;       // 解码器（持有画布与文件副本）
        lv_obj_t *gif_obj = nullptr; // 全画布覆盖层（container 子对象，最上层）
        lv_image_dsc_t gif_dsc{};
        uint32_t gif_next_ms = 0;    // 下一帧的相对时间点（自 action_started_ms）
        bool gif_layers_hidden = false; // GIF 播放期各素材层被隐藏（GIF 透明区会透出底层形象）

        ~Art() {
            for (auto &layer : layers) {
                for (auto &frame : layer.frames)
                    if (frame.pixels) heap_caps_free(frame.pixels);
                for (auto &frame : layer.baked_frames)
                    if (frame.pixels) heap_caps_free(frame.pixels);
            }
        }
    } art;
};

void freeFrames(std::vector<Frame> &frames) {
    for (auto &frame : frames)
        if (frame.pixels) heap_caps_free(frame.pixels);
    frames.clear();
}

// 撤 GIF 动作播放状态。gif_obj 是 container 的子对象：container 将被删除时
// 对象树会带走它，这里统一走 lv_obj_delete 也没有二次释放问题（指针立即置空）。
void gifStop(Parts *parts) {
    auto &art = parts->art;
    if (art.gif_obj && art.container) lv_obj_delete(art.gif_obj);
    art.gif_obj = nullptr;
    art.gif.close();   // 释放画布 + 文件副本
    art.gif_dsc = {};
    art.gif_next_ms = 0;
    if (art.gif_layers_hidden) {
        art.gif_layers_hidden = false;
        for (auto &layer : art.layers)
            if (layer.obj) lv_obj_remove_flag(layer.obj, LV_OBJ_FLAG_HIDDEN);
    }
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
    std::string id;
    std::vector<EmotionSel> mood_sel;   // 有情绪选择时 size = kMoodCount
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
        ESP_LOGE("made_sprite", "[sprite] png_begin_failed path=%s msg=%s",
            path.c_str(), image.message);
        png_image_free(&image);
        return nullptr;
    }
    image.format = PNG_FORMAT_BGRA;
    const size_t bytes = static_cast<size_t>(image.width) * image.height * 4;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) {
        ESP_LOGE("made_sprite", "[sprite] png_alloc_failed path=%s bytes=%u psram_free=%u largest=%u",
            path.c_str(), static_cast<unsigned>(bytes),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        png_image_free(&image);
        return nullptr;
    }
    if (!png_image_finish_read(&image, nullptr, buffer, 0, nullptr)) {
        ESP_LOGE("made_sprite", "[sprite] png_read_failed path=%s msg=%s",
            path.c_str(), image.message);
        png_image_free(&image);
        heap_caps_free(buffer);
        return nullptr;
    }
    width = static_cast<int>(image.width);
    height = static_cast<int>(image.height);
    return buffer;
}

// —— 做法 A：呼吸帧轮播烘焙 ——
// body 层的呼吸缩放（约 ±4%）按等标度步进预烘焙成 K 帧显示尺寸的 ARGB8888
// 位图（源图先裁到内容 bbox 省内存）。运行时逐拍只换 src + 位移补偿，变换
// 域从「每拍重采样整张大图」缩小到「零变换 blit」——滑动/动效卡顿的最大
// 单一来源。烘焙是一次性 CPU（每帧几毫秒），内存按 SPIRAM 预算从 12 帧
// 逐级降到 4 帧，全失败则回退原动态缩放路径。

// 内容 bbox（alpha≥16 视为有内容）。整图透明返回 false。
bool contentBbox(const uint8_t *px, int w, int h, int &bx, int &by, int &bw, int &bh)
{
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = px + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            if (row[x * 4 + 3] >= 16) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx < 0) return false;
    bx = minx; by = miny;
    bw = maxx - minx + 1;
    bh = maxy - miny + 1;
    return true;
}

// 双线性重采样 ARGB8888 的源图子区域（straight alpha：预乘后采样再还原，
// 避免半透明边缘在缩小时发暗）。失败返回 null。
//
// 全程 Q16 定点整数：跨平台/跨核确定性（同输入必同输出），不受编译器
// 浮点行为影响，便于用校验和做两端对账。注意：曾误判条纹花屏是后台
// 任务浮点计算所致，实为 bakeTick 里 region 整块 memcpy 的 stride 错位
// （见调度处注释），float 版本本身没有错。
uint8_t *resampleArgb(const uint8_t *src, int sw, int sh,
                      int sx0, int sy0, int sw_box, int sh_box, int dw, int dh)
{
    if (dw <= 0 || dh <= 0 || sw_box <= 0 || sh_box <= 0 ||
        sx0 < 0 || sy0 < 0 || sx0 + sw_box > sw || sy0 + sh_box > sh)
        return nullptr;
    auto *dst = static_cast<uint8_t *>(
        heap_caps_malloc(static_cast<size_t>(dw) * dh * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!dst) return nullptr;
    // 每输出像素在源图上前进的距离（Q16）
    const int rx_q16 = static_cast<int>((static_cast<int64_t>(sw_box) << 16) / dw);
    const int ry_q16 = static_cast<int>((static_cast<int64_t>(sh_box) << 16) / dh);
    for (int dy = 0; dy < dh; ++dy) {
        // fy = (dy+0.5)*ry - 0.5（Q16）
        const int fy = static_cast<int>((static_cast<int64_t>(2 * dy + 1) * ry_q16 - 65536) >> 1);
        int y0 = fy < 0 ? 0 : (fy >> 16);
        int ty_q8 = fy < 0 ? 0 : ((fy >> 8) & 0xFF);
        if (y0 > sh_box - 1) { y0 = sh_box - 1; ty_q8 = 0; }
        const int y1 = std::min(y0 + 1, sh_box - 1);
        const int w_top_q8 = 256 - ty_q8;
        const uint8_t *row0 = src + static_cast<size_t>(sy0 + y0) * sw * 4;
        const uint8_t *row1 = src + static_cast<size_t>(sy0 + y1) * sw * 4;
        uint8_t *out = dst + static_cast<size_t>(dy) * dw * 4;
        for (int dx = 0; dx < dw; ++dx, out += 4) {
            const int fx = static_cast<int>((static_cast<int64_t>(2 * dx + 1) * rx_q16 - 65536) >> 1);
            int x0 = fx < 0 ? 0 : (fx >> 16);
            int tx_q8 = fx < 0 ? 0 : ((fx >> 8) & 0xFF);
            if (x0 > sw_box - 1) { x0 = sw_box - 1; tx_q8 = 0; }
            const int x1 = std::min(x0 + 1, sw_box - 1);
            const int w_left_q8 = 256 - tx_q8;
            const int w00 = w_left_q8 * w_top_q8;   // Q16（Q8×Q8）
            const int w10 = tx_q8 * w_top_q8;
            const int w01 = w_left_q8 * ty_q8;
            const int w11 = tx_q8 * ty_q8;
            const uint8_t *p00 = row0 + static_cast<size_t>(sx0 + x0) * 4;
            const uint8_t *p10 = row0 + static_cast<size_t>(sx0 + x1) * 4;
            const uint8_t *p01 = row1 + static_cast<size_t>(sx0 + x0) * 4;
            const uint8_t *p11 = row1 + static_cast<size_t>(sx0 + x1) * 4;
            const int acc_a = w00 * p00[3] + w10 * p10[3] + w01 * p01[3] + w11 * p11[3];
            out[3] = static_cast<uint8_t>(acc_a >> 16);
            if (acc_a > 65) {   // ≈ alpha 0.001 阈值
                // 预乘颜色：Σ w·C·A / acc_a（int64 防溢出：w·C·A ≤ 65536·65025）
                for (int c = 0; c < 3; ++c) {
                    const int64_t prem = static_cast<int64_t>(w00) * p00[c] * p00[3] +
                        static_cast<int64_t>(w10) * p10[c] * p10[3] +
                        static_cast<int64_t>(w01) * p01[c] * p01[3] +
                        static_cast<int64_t>(w11) * p11[c] * p11[3];
                    out[c] = static_cast<uint8_t>(std::min<int64_t>(
                        (prem + acc_a / 2) / acc_a, 255));
                }
            } else {
                out[0] = out[1] = out[2] = 0;
            }
        }
    }
    return dst;
}

// —— 呼吸帧烘焙（后台化）——
// 每次进出屏幕都会重建舞台并重新提交素材，同步烘焙 K 帧（PSRAM 双线性重采样）
// 会把 LVGL 线程卡住约 2 秒（实机「每次换屏都卡两秒」的根因）。改为三段式：
//   1) bakeTick（LVGL 线程）：给候选层规划帧数/缩放表，把源图内容 bbox 区域
//      拷成私有副本后交给后台任务——副本与 LVGL 侧像素彻底解耦，素材中途
//      换装/舞台销毁都不会让后台任务读到已释放内存。
//   2) bakeWorker（后台任务）：逐帧重采样，只碰私有副本与新分配的 PSRAM，
//      完成后置 done。
//   3) bakeTick 收割：epoch 对账通过才把结果挂到层上（原子切到帧轮播），
//      否则整体丢弃。烘焙期间动态缩放路径照常兜底显示。
//
// 预算：SPIRAM 空闲的一半且 ≤3.5MB（给 LVGL 缓冲/网络留余量），且最大
// 单帧不超过最大连续块（逐帧独立分配）。等标度步进：k 帧
// s_k = 256 - amp + round(2*amp*k/(M-1))，覆盖 [256-amp, 256+amp]。
bool planBakeScales(int bw, int bh, int base_scale, int amp, std::vector<int> &scales)
{
    const size_t free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    // 预取预留：滑动方向预取随时可能要在后台解码第二套素材（2~3MB）。
    // 烘焙若把空闲池吃穿（旧预算 free/2 上限 3.5MB 就会这样），翻回去的
    // 那一屏预取永远过不了 1.5MB 守卫——素材只能翻页后才出现。恒留
    // 2.5MB：烘焙拿不到就少烘几帧或干脆跳过（动态缩放兜底），比锁死
    // 预取划算。
    const size_t reserve = static_cast<size_t>(2560) * 1024;
    const size_t budget = std::min(free_size > reserve ? free_size - reserve : 0,
                                   static_cast<size_t>(3500) * 1024);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    for (int M : {12, 10, 8, 6, 4}) {
        size_t total = 0;
        bool fits = true;
        int max_frame = 0;
        scales.clear();
        for (int k = 0; k < M && fits; ++k) {
            const int s = 256 - amp + (2 * amp * k + (M - 1) / 2) / (M - 1);
            const int dw = std::max(1, static_cast<int>((static_cast<int64_t>(bw) * base_scale * s + 32768) / 65536));
            const int dh = std::max(1, static_cast<int>((static_cast<int64_t>(bh) * base_scale * s + 32768) / 65536));
            const int bytes = dw * dh * 4;
            total += static_cast<size_t>(bytes);
            max_frame = std::max(max_frame, bytes);
            scales.push_back(s);
            fits = total <= budget && static_cast<size_t>(max_frame) <= largest / 2;
        }
        if (fits) return true;
    }
    scales.clear();
    return false;
}

struct BakeState {
    std::atomic<bool> in_flight{false};
    std::atomic<bool> done{false};
    // 调度时（LVGL 线程）写入，worker 只读：
    uint8_t *region = nullptr;   // bbox 内容区的私有副本（worker 的唯一输入）
    int bw = 0, bh = 0;
    int base_scale = 256;
    std::vector<int> scales;
    uint64_t epoch = 0;
    uint32_t layer_index = 0;
    int bbox_x = 0, bbox_y = 0;
    // 结果（worker 写，done 后 LVGL 线程收割/丢弃）：
    std::vector<Frame> frames;
    std::vector<uint32_t> checksums;   // worker 侧逐帧 FNV-1a（诊断用）
    uint32_t region_sum = 0;           // worker 开工前对 region 再算一次
};
BakeState g_bake;
std::atomic<uint64_t> g_art_epoch{1};

uint32_t fnv1a(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

void bakeWorker(void *)
{
    g_bake.region_sum = fnv1a(g_bake.region,
        static_cast<size_t>(g_bake.bw) * g_bake.bh * 4);
    for (size_t k = 0; k < g_bake.scales.size(); ++k) {
        const int s = g_bake.scales[k];
        const int dw = std::max(1, static_cast<int>((static_cast<int64_t>(g_bake.bw) * g_bake.base_scale * s + 32768) / 65536));
        const int dh = std::max(1, static_cast<int>((static_cast<int64_t>(g_bake.bh) * g_bake.base_scale * s + 32768) / 65536));
        auto *px = resampleArgb(g_bake.region, g_bake.bw, g_bake.bh,
                                0, 0, g_bake.bw, g_bake.bh, dw, dh);
        if (!px) break;   // 内存吃紧：帧数就到这，短表也照常可用
        g_bake.checksums.push_back(fnv1a(px, static_cast<size_t>(dw) * dh * 4));
        Frame f;
        f.pixels = px;
        f.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        f.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        f.dsc.header.w = static_cast<uint32_t>(dw);
        f.dsc.header.h = static_cast<uint32_t>(dh);
        f.dsc.header.stride = static_cast<uint32_t>(dw) * 4;
        f.dsc.data_size = static_cast<size_t>(dw) * dh * 4;
        f.dsc.data = px;
        g_bake.frames.push_back(f);
    }
    ESP_LOGD("made_sprite", "[bake] worker done frames=%u region=%08x",
        static_cast<unsigned>(g_bake.frames.size()),
        static_cast<unsigned>(g_bake.region_sum));
    g_bake.done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

// LVGL 线程，update() 每次调用（先于 10Hz 节流门）。
void bakeTick(Parts *parts)
{
    auto &art = parts->art;
    if (!art.active) return;
    if (!parts->bake_enabled) return;   // 侧舞台：动画走动态缩放，不抢烘焙

    // 收割：done 后结果所有权移到局部，epoch 不匹配就地析构释放。
    if (g_bake.in_flight.load(std::memory_order_acquire) &&
        g_bake.done.load(std::memory_order_acquire)) {
        std::vector<Frame> frames = std::move(g_bake.frames);
        std::vector<int> scales = std::move(g_bake.scales);
        g_bake.frames.clear();
        g_bake.scales.clear();
        heap_caps_free(g_bake.region);
        g_bake.region = nullptr;
        g_bake.done.store(false, std::memory_order_relaxed);
        g_bake.in_flight.store(false, std::memory_order_release);
        std::vector<uint32_t> checksums = std::move(g_bake.checksums);
        g_bake.checksums.clear();
        if (g_bake.epoch == art.epoch && g_bake.layer_index < art.layers.size() &&
            !frames.empty()) {
            ArtLayer &layer = art.layers[g_bake.layer_index];
            if (!layer.baked && layer.obj) {
                // 诊断：收割时重算第 0 帧校验和，与 worker 侧对比。不一致 =
                // 双核/缓存可见性问题，弃用烘焙保动态缩放（绝不上屏坏数据）。
                const Frame &f0 = frames.front();
                const uint32_t now_sum = fnv1a(
                    static_cast<const uint8_t *>(f0.dsc.data), f0.dsc.data_size);
                const uint32_t worker_sum = checksums.empty() ? 0 : checksums.front();
                if (now_sum != worker_sum) {
                    ESP_LOGW("made_sprite", "[bake] checksum mismatch, keep dynamic");
                    return;
                }
                layer.bbox_x = g_bake.bbox_x;
                layer.bbox_y = g_bake.bbox_y;
                layer.baked_frames = std::move(frames);
                layer.baked_scale = std::move(scales);
                layer.baked = true;
                // 原帧不再参与显示，释放省几百 KB PSRAM（内置豆的静态 dsc
                // 没有堆像素，freeFrames 自然跳过）。
                freeFrames(layer.frames);
                // 烘焙帧已是显示尺寸：缩放固定 1:1，后续只换 src + 位移。
                // 用 255 而不是 256：256 可能走 LVGL 的恒等（identity）绘制
                // 快路径，其真机表现未经独立验证；255 强制走变换绘制路径，
                // 视觉差 0.4% 不可感知（条纹根因已查明是 region 拷贝 bug，
                // 与此无关，此处仅作稳妥保留）。
                lv_image_set_scale(layer.obj, 255);
                lv_image_set_src(layer.obj, &layer.baked_frames.front().dsc);
                layer.last_frame = 0;
                lv_obj_invalidate(parts->root);
                ESP_LOGI("made_sprite", "[bake] applied frames=%u scale=[%d..%d]",
                    static_cast<unsigned>(layer.baked_frames.size()),
                    layer.baked_scale.front(), layer.baked_scale.back());
            }
        }
    }

    // 调度：找最大的可烘焙单帧层（无 blink/talk/alert，未烘焙、未试败）。
    // 行动系统引用的层不烘焙：烘焙会释放原帧并锁死帧轮播路径，动作切帧
    // 与烘焙帧轮播会互相打架。
    if (g_bake.in_flight.load(std::memory_order_acquire)) return;
    ArtLayer *body = nullptr;
    uint32_t area = 0;
    for (size_t li = 0; li < art.layers.size(); ++li) {
        ArtLayer &layer = art.layers[li];
        if (layer.baked || layer.bake_tried || layer.blink || layer.talk || layer.alert)
            continue;
        if (li < art.action_layers.size() && art.action_layers[li]) continue;
        if (layer.frames.size() != 1 || !layer.frames[0].dsc.data) continue;
        const uint32_t a = layer.frames[0].dsc.header.w * layer.frames[0].dsc.header.h;
        if (a > area) { area = a; body = &layer; }
    }
    if (!body) return;
    body->bake_tried = true;   // 无论成败本轮素材只试一次

    const int amp = (art.breath_pct - 100) * 256 / 200;
    if (amp < 2) return;
    Frame &srcf = body->frames[0];
    const int sw = static_cast<int>(srcf.dsc.header.w);
    const int sh = static_cast<int>(srcf.dsc.header.h);
    int bx, by, bw, bh;
    if (sw <= 0 || sh <= 0 || !contentBbox(srcf.dsc.data, sw, sh, bx, by, bw, bh)) return;

    std::vector<int> scales;
    if (!planBakeScales(bw, bh, body->base_scale, amp, scales)) {
        ESP_LOGW("made_sprite", "[bake] skipped (budget) bbox=%dx%d free=%uKB", bw, bh,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return;
    }
    // bbox 区域私有副本（PSRAM→PSRAM，≤0.5MB 逐行 memcpy）。
    // 注意必须逐行拷：bbox 在整幅源图里每行之间隔着 (sw-bw) 像素不连续
    // 数据，整块 memcpy 会把行尾拼上下行开头、逐行漂移 (sw-bw)*4 字节，
    // 渲染成斜向剪切条纹（真机与 sim 同错，校验和一致但内容本身是错的——
    // 「数据校验正确」不等于「拷贝正确」，校验只能证自洽不能证对）。
    auto *region = static_cast<uint8_t *>(heap_caps_malloc(
        static_cast<size_t>(bw) * bh * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!region) return;
    {
        const uint8_t *src = static_cast<const uint8_t *>(srcf.dsc.data);
        const size_t sstride = srcf.dsc.header.stride
            ? static_cast<size_t>(srcf.dsc.header.stride)
            : static_cast<size_t>(sw) * 4;
        for (int y = 0; y < bh; ++y) {
            std::memcpy(region + static_cast<size_t>(y) * bw * 4,
                src + static_cast<size_t>(by + y) * sstride + static_cast<size_t>(bx) * 4,
                static_cast<size_t>(bw) * 4);
        }
    }
    g_bake.region = region;
    g_bake.bw = bw;
    g_bake.bh = bh;
    g_bake.base_scale = body->base_scale;
    g_bake.scales = std::move(scales);
    g_bake.epoch = art.epoch;
    g_bake.layer_index = static_cast<uint32_t>(body - art.layers.data());
    g_bake.bbox_x = bx;
    g_bake.bbox_y = by;
    g_bake.frames.clear();
    g_bake.in_flight.store(true, std::memory_order_release);
    if (xTaskCreate(bakeWorker, "sprite_bake", 4096, nullptr, 1, nullptr) != pdPASS) {
        // 起不来任务：恢复现场，退回动态缩放
        g_bake.in_flight.store(false, std::memory_order_release);
        heap_caps_free(region);
        g_bake.region = nullptr;
        g_bake.scales.clear();
    }
}

// 把规格提交成 LVGL 对象树；任一对象创建失败则整体回滚并保留旧素材。
// actions 是 manifest 解析出的动作表：层引用在这里解析成下标，引用不存在
// 的层或越界帧的轨道就地修剪——动作可用性由素材本身决定，坏条目只影响
// 自己（缺素材的动作不可用，其余照常）。
bool commitArt(Parts *parts, const MotionSpec &motion, std::vector<LayerSpec> &&specs,
               std::vector<ActionSpec> &&actions, bool builtin, const std::string &base_dir) {
    if (specs.empty()) return false;
    gifStop(parts);   // 上一套素材的 GIF 播放状态先行退场（含覆盖层与解码内存）
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
        layer.id = std::move(spec.id);
        layer.mood_sel = std::move(spec.mood_sel);
        layer.has_emotions = !layer.mood_sel.empty();
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
    for (auto &layer : parts->art.layers) {
        freeFrames(layer.frames);
        freeFrames(layer.baked_frames);
    }
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
    parts->art.base_dir = base_dir;
    // —— 动作表落地：解析层下标 + 修剪越界帧 ——
    parts->art.actions.clear();
    parts->art.action_layers.assign(parts->art.layers.size(), 0);
    for (auto &action : actions) {
        ActionSpec resolved;
        resolved.name = std::move(action.name);
        resolved.frame_ms = action.frame_ms;
        if (action.gif) {
            // GIF 动作：无轨道可解析；可用性由文件在不在缓存里决定
            // （playAction 时读不到文件就静默降级）。
            resolved.gif = true;
            resolved.gif_file = std::move(action.gif_file);
            resolved.gif_ms = action.gif_ms;
            parts->art.actions.push_back(std::move(resolved));
            continue;
        }
        uint32_t max_len = 0;
        for (auto &track : action.tracks) {
            ssize_t index = -1;
            for (size_t i = 0; i < parts->art.layers.size(); ++i) {
                if (parts->art.layers[i].id == track.layer) { index = static_cast<ssize_t>(i); break; }
            }
            if (index < 0) continue;   // 引用不存在的层：整条轨道丢弃
            ActionSpec::Track out;
            out.index = static_cast<size_t>(index);
            for (const int frame : track.frames) {
                if (frame < static_cast<int>(parts->art.layers[out.index].frames.size()))
                    out.frames.push_back(frame);
            }
            if (out.frames.empty()) continue;
            max_len = std::max<uint32_t>(max_len, static_cast<uint32_t>(out.frames.size()));
            parts->art.action_layers[out.index] = 1;
            resolved.tracks.push_back(std::move(out));
        }
        if (resolved.tracks.empty()) continue;   // 没有一条可用轨道：整个动作不可用
        resolved.total_steps = max_len * action.loops;
        parts->art.actions.push_back(std::move(resolved));
    }
    parts->art.active_action = -1;   // 换素材即停动作（新素材的动作表是另一套）
    // 烘焙不在提交时同步做（那会把 LVGL 线程卡约 2 秒，实机每次换屏都卡）。
    // 这里只递增代际号，update() 里的 bakeTick 会异步调度后台烘焙；
    // 烘焙期间动态缩放路径照常兜底，烘完原子切帧轮播。
    parts->art.epoch = g_art_epoch.fetch_add(1) + 1;

    for (const auto &layer : parts->art.layers) {
        ESP_LOGI("made_sprite", "[art] layer id=%s frames=%u blink=%d emotions=%d period=%u closed=%u",
            layer.id.c_str(), static_cast<unsigned>(layer.frames.size()), layer.blink ? 1 : 0,
            layer.has_emotions ? 1 : 0,
            static_cast<unsigned>(layer.blink_period), static_cast<unsigned>(layer.blink_closed));
    }
    lv_obj_invalidate(parts->root);
    return true;

fail:
    // 只回收本轮已建成的层：旧素材与旧层表保持可用，等下一次提交再换。
    ESP_LOGE("made_sprite", "[sprite] commit_failed layers=%u psram_free=%u",
        static_cast<unsigned>(layers.size()),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    for (auto &layer : layers) {
        freeFrames(layer.frames);
        freeFrames(layer.baked_frames);
    }
    lv_obj_delete(container);
    return false;
}

void destroy(lv_event_t *event) {
    auto *parts = static_cast<Parts *>(lv_event_get_user_data(event));
    lv_obj_set_user_data(lv_event_get_target_obj(event), nullptr);
    delete parts;
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
    } else if (mood == Mood::kSuccess) {
        sway_period = sway_period * 5 / 6;   // 完成：轻快一点
        bounce_amp += 3;
    } else if (mood == Mood::kSad) {
        sway_period = sway_period * 7 / 6;   // 失败：蔫一点
        sway_amp -= 2;
    }
    const int sway = wave(elapsed_ms, sway_period, sway_amp);
    const int bounce = (wave(elapsed_ms, sway_period / 2, bounce_amp, 270) + bounce_amp) / 2;
    lv_obj_set_pos(art.container, made_x(sway), made_home_y(-bounce));

    // —— 行动系统：播放中的动作覆盖被引用层的帧选择 ——
    // 时间轴用 lv_tick_get()（playAction 也在 LVGL 线程记录起点，同源无漂移）。
    // 播完自动清位，后续层照常走情绪/状态驱动。烘焙层不可能是动作目标
    // （调度处已排除 action_layers），此处 override 与帧轮播不会相遇。
    int playing = -1;
    uint32_t action_step = 0;
    bool gif_playing = false;
    if (art.active_action >= 0 &&
        art.active_action < static_cast<int>(art.actions.size())) {
        const ActionSpec &spec = art.actions[static_cast<size_t>(art.active_action)];
        const uint32_t t = static_cast<uint32_t>(lv_tick_get()) - art.action_started_ms;
        bool expired;
        if (spec.gif) {
            expired = t >= spec.gif_ms;
            if (!expired) {
                // 逐帧流式解码：到点才解下一帧（LVGL 线程内，每帧几毫秒）。
                // 播完 GIF 流但时长未到：rewind 循环续播。
                if (t >= art.gif_next_ms) {
                    uint32_t frame_ms = 0;
                    int got = art.gif.decodeNext(frame_ms);
                    if (got == 0) {
                        art.gif.rewind();
                        got = art.gif.decodeNext(frame_ms);
                    }
                    if (got != 1) {
                        expired = true;   // 解码失败：立即结束，不留残帧
                    } else {
                        art.gif_next_ms = t + frame_ms;
                        lv_image_cache_drop(&art.gif_dsc);   // 像素变了，弃缓存
                        lv_obj_invalidate(art.gif_obj);
                    }
                }
            }
        } else {
            expired = spec.total_steps == 0 || t >= spec.total_steps * spec.frame_ms;
        }
        if (expired) {
            if (spec.gif) gif_playing = false;
            art.active_action = -1;   // 播完：回到状态驱动
            gifStop(parts);
        } else {
            playing = art.active_action;
            gif_playing = spec.gif;
            action_step = t / spec.frame_ms;
        }
    }
    std::vector<uint8_t> overridden;
    if (playing >= 0 && !gif_playing) {
        const ActionSpec &spec = art.actions[static_cast<size_t>(playing)];
        overridden.assign(art.layers.size(), 0);
        for (const auto &track : spec.tracks) {
            if (track.index >= art.layers.size()) continue;
            ArtLayer &layer = art.layers[track.index];
            if (!layer.obj || layer.frames.empty()) continue;
            const int frame = std::min(
                track.frames[static_cast<size_t>(action_step) % track.frames.size()],
                static_cast<int>(layer.frames.size()) - 1);
            if (frame != layer.last_frame) layer.last_frame = frame;
            lv_image_set_src(layer.obj, &layer.frames[static_cast<size_t>(frame)].dsc);
            overridden[track.index] = 1;
        }
    }
    // 呼吸：逐层图像缩放 + 位置补偿，支点=画布中心。缩放比 s 时层的位置
    // 必须从 base_pos 变为 center + (base_pos - center) * s —— 等价于容器级
    // 变换的几何效果，但变换域从整屏缩小到各图层自身 bbox，也不再走整屏
    // 重采样（此前动效一卡一卡的主源）。此前 bisect 排查层间漂移时临时
    // 关过呼吸，根因是逐层 pivot 与层表残留（均已修），恢复之。
    const int amp = (art.breath_pct - 100) * 256 / 200;
    const int scale = 256 + wave(elapsed_ms, art.breath_period, amp);
    for (auto &layer : art.layers) {
        if (!layer.obj) continue;
        if (layer.baked && !layer.baked_frames.empty()) {
            // 帧轮播：把本拍理想缩放映射到最近的烘焙帧，换 src + 位移补偿，
            // 零重采样。等标度烘焤，相邻帧尺寸差恒定，过渡均匀。
            const int n = static_cast<int>(layer.baked_frames.size());
            int k = amp > 0 ? (scale - (256 - amp)) * (n - 1) / (2 * amp) : (n - 1) / 2;
            k = std::min(std::max(k, 0), n - 1);
            const int s_k = layer.baked_scale[static_cast<size_t>(k)];
            const int fx = art.pivot_x + (layer.base_x - art.pivot_x) * s_k / 256 +
                static_cast<int>(static_cast<int64_t>(layer.bbox_x) * layer.base_scale * s_k / 65536);
            const int fy = art.pivot_y + (layer.base_y - art.pivot_y) * s_k / 256 +
                static_cast<int>(static_cast<int64_t>(layer.bbox_y) * layer.base_scale * s_k / 65536);
            lv_obj_set_pos(layer.obj, fx, fy);
            if (k != layer.last_frame) {
                layer.last_frame = k;
                lv_image_set_src(layer.obj, &layer.baked_frames[static_cast<size_t>(k)].dsc);
            }
            continue;
        }
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
    for (size_t li = 0; li < art.layers.size(); ++li) {
        ArtLayer &layer = art.layers[li];
        // 动作播放中被引用的层已由动作接管，跳过常规选帧
        if (li < overridden.size() && overridden[li]) continue;
        // 烘焙层的 src 由上面的帧轮播管理，这里只处理 blink/talk/alert 切帧层
        if (!layer.obj || layer.baked || layer.frames.empty()) continue;
        static const EmotionSel kNone;
        const EmotionSel &sel = layer.mood_sel.size() == made_sprite::Mood::kMoodCount
            ? layer.mood_sel[static_cast<size_t>(mood)] : kNone;
        const int frame_count = static_cast<int>(layer.frames.size());
        int frame = 0;
        if (layer.blink) {
            // 眨眼 = 选中帧 ↔ 闭眼帧（frames[1]，素材规范约定）。无情绪表时
            // 选中帧恒为 0；有情绪表时待机 mood 保留眨眼节律（选中帧不是
            // 闭眼帧本身才眨），其余情绪固定在选中帧（睡觉/惊讶不再眨）。
            const int open = sel.frame >= 0 ? sel.frame : 0;
            const int closed = frame_count > 1 ? 1 : open;
            if (sel.frame >= 0 && mood != Mood::kIdle) {
                frame = open;
            } else if (open != closed &&
                       (elapsed_ms % layer.blink_period) < layer.blink_closed) {
                frame = closed;
            } else {
                frame = open;
            }
        } else if (layer.talk && (mood == Mood::kTalking || mood == Mood::kRunning)) {
            // 说话循环全部帧；情绪表可指定固定帧（如惊讶张嘴不循环）
            if (sel.frame >= 0 && !sel.cycle) {
                frame = sel.frame;
            } else {
                frame = static_cast<int>((elapsed_ms / 180) % static_cast<uint32_t>(frame_count));
            }
        } else if (sel.frame >= 0) {
            frame = sel.frame;
        } else if (sel.cycle) {
            frame = static_cast<int>((elapsed_ms / 180) % static_cast<uint32_t>(frame_count));
        }
        frame = std::min(std::max(frame, 0), frame_count - 1);
        if (frame != layer.last_frame) {
            layer.last_frame = frame;
            ESP_LOGD("made_sprite", "[frame] blink=%d talk=%d mood=%d frame=%d elapsed=%u",
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

// 内存 PNG 字节解码为 ARGB8888（与文件版 decodePngArgb 同一管线，供图片附件用）。
// 必须在匿名命名空间之外（made_sprite 命名空间级别），否则符号变为内部链接，
// 与 made_sprite.hpp 的外部声明不匹配，链接器报 undefined。
uint8_t *decodePngArgbMem(const uint8_t *data, size_t size, int &width, int &height) {
    if (!data || size < 8) return nullptr;
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data, size)) {
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

    return root;
}

void update(lv_obj_t *root, uint32_t elapsed_ms, int mood) {
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->art.active) return;
    // 动效 10Hz 节流：渲染泵 20Hz，但呼吸/轻摆/眨眼都是 1.4–4s 的慢波，
    // 10Hz 采样视觉无差；而每拍动效都会重采样全部图层（ARGB8888 软件缩放）
    // 并整屏失效——这是滑动不跟手、整体掉帧的最大单一来源，直接砍半。
    // 情绪切换不受节流，立即反映。
    bakeTick(parts);   // 烘焙收割/调度（有原子门，常驻开销近乎为零）
    // 动作播放不走 10Hz 节流：动作帧是纯 src 切换（无重采样），且动作
    // 步长可以短到 30ms，90ms 节流会把快动作拖成慢动作。
    const bool action_live = parts->art.active_action >= 0;
    if (action_live || mood != parts->last_mood || elapsed_ms - parts->last_anim_ms >= 90) {
        parts->last_anim_ms = elapsed_ms;
        artUpdate(parts, elapsed_ms, mood);
        if (mood != parts->last_mood) {
            parts->last_mood = mood;
            lv_obj_invalidate(root);
        }
    }
}

// manifest JSON → 动效参数 + 图层帧规格（含全部 PNG 解码到 PSRAM）。
// 任一帧解码失败返回 false，已解码的帧就地释放。applyArtwork（LVGL 线程
// 兜底路径）与滑动预取 worker 共用；本函数不碰 LVGL 对象，可在任意线程跑。
// static：签名引用匿名命名空间类型，保持内部链接。
static bool buildSpecsFromManifest(const std::string &manifest_json, const std::string &base_dir,
                                   MotionSpec &motion, std::vector<LayerSpec> &specs,
                                   std::vector<ActionSpec> &actions) {
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
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(layer, "id");
        if (cJSON_IsString(id) && id->valuestring) spec.id = id->valuestring;
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

    // —— 情绪表（可选）——
    // "emotions": { "<mood>": { "<layerId>": <帧序号 | "cycle"> } }。
    // mood 名与 Mood::Value 一一对应（idle/talking/running/waiting/success/
    // sad/offline）。帧序号越界或引用未知层：丢弃该条（桥接器已做严格校验，
    // 这里宽容处理，不让单个坏条目毁掉整套素材）。
    {
        static const struct { const char *name; int mood; } kMoods[] = {
            {"idle", Mood::kIdle},           {"talking", Mood::kTalking},
            {"running", Mood::kRunning},     {"waiting", Mood::kWaitingConfirm},
            {"success", Mood::kSuccess},     {"sad", Mood::kSad},
            {"offline", Mood::kOffline},
        };
        const cJSON *emotions = cJSON_GetObjectItemCaseSensitive(manifest, "emotions");
        if (cJSON_IsObject(emotions)) {
            for (auto &entry : kMoods) {
                const cJSON *mood_entry =
                    cJSON_GetObjectItemCaseSensitive(emotions, entry.name);
                if (!cJSON_IsObject(mood_entry)) continue;
                for (const cJSON *sel = mood_entry->child; sel; sel = sel->next) {
                    if (!sel->string) continue;
                    LayerSpec *target = nullptr;
                    for (auto &spec : specs)
                        if (spec.id == sel->string) { target = &spec; break; }
                    if (!target) continue;
                    if (target->mood_sel.empty())
                        target->mood_sel.assign(Mood::kMoodCount, EmotionSel{});
                    EmotionSel &slot =
                        target->mood_sel[static_cast<size_t>(entry.mood)];
                    if (cJSON_IsString(sel) && sel->valuestring &&
                        std::strcmp(sel->valuestring, "cycle") == 0) {
                        slot.cycle = true;
                    } else if (cJSON_IsNumber(sel)) {
                        if (sel->valueint >= 0 &&
                            sel->valueint < static_cast<int>(target->frames.size()))
                            slot.frame = static_cast<int8_t>(sel->valueint);
                    }
                }
            }
        }
    }

    // —— 行动表（可选）——
    // "actions": { "<name>": { "frame_ms": 120, "loops": 1,
    //                           "tracks": { "<layerId>": [帧序号...] } } }
    // 名字规则与桥接器能力名一致（小写字母开头，[a-z0-9_]，≤16 字符）；
    // 层引用/帧越界在 commitArt 解析时修剪——素材没给齐的动作自然不可用，
    // 坏条目只影响自己，不毁整套素材。
    if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(manifest, "actions"))) {
        const cJSON *actions_json = cJSON_GetObjectItemCaseSensitive(manifest, "actions");
        for (const cJSON *action = actions_json->child; action; action = action->next) {
            if (!action->string || !cJSON_IsObject(action) || actions.size() >= 16) continue;
            const std::string name = action->string;
            if (name.size() > 16 || name[0] < 'a' || name[0] > 'z' ||
                name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_", 1) != std::string::npos)
                continue;
            ActionSpec spec;
            spec.name = name;
            // GIF 动作形态：{ "gif": "x.gif", "ms": 播放时长 }
            const cJSON *gif = cJSON_GetObjectItemCaseSensitive(action, "gif");
            if (cJSON_IsString(gif) && gif->valuestring && gif->valuestring[0]) {
                const std::string file = gif->valuestring;
                if (file.size() > 20 || file.size() < 5 ||
                    file.compare(file.size() - 4, 4, ".gif") != 0 ||
                    file.find_first_of("/\\") != std::string::npos ||
                    file.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
                        std::string::npos)
                    continue;   // 名字不合规：动作不可用，不影响其它素材
                spec.gif = true;
                spec.gif_file = file;
                const cJSON *ms = cJSON_GetObjectItemCaseSensitive(action, "ms");
                spec.gif_ms = cJSON_IsNumber(ms)
                    ? static_cast<uint32_t>(std::min(std::max(ms->valueint, 100), 10000))
                    : 3000;
                actions.push_back(std::move(spec));
                continue;
            }
            const cJSON *frame_ms = cJSON_GetObjectItemCaseSensitive(action, "frame_ms");
            if (cJSON_IsNumber(frame_ms) && frame_ms->valueint >= 30 && frame_ms->valueint <= 2000)
                spec.frame_ms = static_cast<uint32_t>(frame_ms->valueint);
            const cJSON *loops = cJSON_GetObjectItemCaseSensitive(action, "loops");
            spec.loops = cJSON_IsNumber(loops) ? std::min(std::max(loops->valueint, 1), 10) : 1;
            const cJSON *tracks = cJSON_GetObjectItemCaseSensitive(action, "tracks");
            if (!cJSON_IsObject(tracks)) continue;
            bool valid = false;
            for (const cJSON *track = tracks->child; track; track = track->next) {
                if (!track->string || !cJSON_IsArray(track) || spec.tracks.size() >= 8) continue;
                ActionSpec::Track out;
                out.layer = track->string;
                for (const cJSON *frame = track->child; frame && out.frames.size() < 32; frame = frame->next)
                    if (cJSON_IsNumber(frame) && frame->valueint >= 0)
                        out.frames.push_back(frame->valueint);
                if (out.frames.empty()) continue;
                valid = true;
                spec.tracks.push_back(std::move(out));
            }
            if (valid) actions.push_back(std::move(spec));
        }
    }

    cJSON_Delete(manifest);
    return true;
fail:
    for (auto &spec : specs) freeFrames(spec.frames);
    cJSON_Delete(manifest);
    return false;
}

bool applyArtwork(lv_obj_t *root, const std::string &manifest_json, const std::string &base_dir) {
    if (!root) return false;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->root) return false;
    // 先快速校验 manifest 合法（完整解析在 buildSpecsFromManifest 里再做，
    // 纯解析开销可忽略）——manifest 不合法时不能撤旧素材。
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
    cJSON_Delete(manifest);

    // 旧素材先行退场：新帧若在旧帧还活着时解码，两套 324×324×ARGB 叠加
    // 会把 PSRAM 峰值推到双倍（实机切换时解码失败回退空白）。先撤旧层，
    // 解码峰值只剩新素材一套；失败时本来就停在空白等下次重试。
    // （滑动预取路径不受此限：预取是「旧素材存活时后台解码」，PSRAM 放得
    // 下就赚到零等待换装，放不下解码失败回退到这里，行为与现在一致。）
    gifStop(parts);
    if (parts->art.container) lv_obj_delete(parts->art.container);
    parts->art.container = nullptr;
    for (auto &layer : parts->art.layers) {
        freeFrames(layer.frames);
        freeFrames(layer.baked_frames);
    }
    parts->art.layers.clear();
    parts->art.active = false;

    MotionSpec motion;
    std::vector<LayerSpec> specs;
    std::vector<ActionSpec> actions;
    if (!buildSpecsFromManifest(manifest_json, base_dir, motion, specs, actions)) return false;

    if (commitArt(parts, motion, std::move(specs), std::move(actions), false, base_dir)) {
        lv_obj_invalidate(root);
        return true;
    }
    return false;
}

void clearArtwork(lv_obj_t *root) {
    if (!root) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->art.active || parts->builtin) return;
    gifStop(parts);
    if (parts->art.container) lv_obj_delete(parts->art.container);
    parts->art.container = nullptr;
    for (auto &layer : parts->art.layers) {
        freeFrames(layer.frames);
        freeFrames(layer.baked_frames);
    }
    parts->art.layers.clear();
    parts->art.active = false;
    // 内置绿豆已删除：素材撤下后舞台留空，等待下一套素材。
}

bool artworkActive(lv_obj_t *root) {
    if (!root) return false;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    return parts && parts->art.active && !parts->builtin;
}

// 读整个缓存文件（GIF 动作用；≤256KB 一把读进 PSRAM）。失败返回空串。
static std::string readCachedFile(const std::string &path, size_t limit) {
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) return {};
    std::string out;
    char buffer[4096];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, read);
        if (out.size() > limit) break;
    }
    fclose(file);
    return out;
}

bool playAction(lv_obj_t *root, const char *name) {
    if (!root || !name || !name[0]) return false;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->art.active) return false;
    auto &art = parts->art;
    for (size_t i = 0; i < art.actions.size(); ++i) {
        if (art.actions[i].name != name) continue;
        // 重复点名同一段动作：重新从头播（打断当前拍比忽略更符合直觉）
        if (art.actions[i].gif) {
            // GIF 动作：读缓存文件 → 起解码器 → 建全画布覆盖层 → 解首帧
            gifStop(parts);
            if (art.actions[i].gif_file.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
                std::string::npos)
                return false;   // 文件名越界：静默降级
            const std::string bytes = readCachedFile(
                art.base_dir + art.actions[i].gif_file, 256 * 1024 + 1);
            if (bytes.empty() || bytes.size() > 256 * 1024) {
                ESP_LOGW("made_sprite", "[action] gif_unavailable name=%s bytes=%u",
                    name, static_cast<unsigned>(bytes.size()));
                return false;
            }
            if (!art.gif.open(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size())) {
                ESP_LOGW("made_sprite", "[action] gif_open_failed name=%s", name);
                return false;
            }
            auto *object = lv_image_create(art.container);
            if (!object) {
                gifStop(parts);
                return false;
            }
            passive(object);
            art.gif_obj = object;
            art.gif_dsc = {};
            art.gif_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            art.gif_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
            art.gif_dsc.header.w = static_cast<uint32_t>(art.gif.w);
            art.gif_dsc.header.h = static_cast<uint32_t>(art.gif.h);
            art.gif_dsc.header.stride = static_cast<uint32_t>(art.gif.w) * 4;
            art.gif_dsc.data_size = static_cast<size_t>(art.gif.w) * art.gif.h * 4;
            art.gif_dsc.data = art.gif.canvas;
            lv_image_set_src(object, &art.gif_dsc);
            lv_obj_set_pos(object, made_x(0), made_home_y(0));
            lv_image_set_scale(object, made_lv_scale());
            lv_image_set_pivot(object, 0, 0);
            uint32_t frame_ms = 100;
            if (art.gif.decodeNext(frame_ms) != 1) {
                ESP_LOGW("made_sprite", "[action] gif_first_frame_failed name=%s", name);
                gifStop(parts);
                return false;
            }
            art.gif_next_ms = frame_ms;
            // GIF 透明区会透出下面的分层形象（双轮廓鬼影）：播放期整体隐藏素材层
            art.gif_layers_hidden = true;
            for (auto &layer : art.layers)
                if (layer.obj) lv_obj_add_flag(layer.obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(object);
            lv_obj_invalidate(art.container);
        } else {
            if (art.actions[i].total_steps == 0) return false;
        }
        art.active_action = static_cast<int>(i);
        art.action_started_ms = static_cast<uint32_t>(lv_tick_get());
        ESP_LOGI("made_sprite", "[action] play name=%s", name);
        return true;
    }
    // 素材没这个动作：静默降级（不报错、不动），调用方无需处理
    return false;
}

std::vector<std::string> actionNames(lv_obj_t *root) {
    std::vector<std::string> names;
    if (!root) return names;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->art.active) return names;
    names.reserve(parts->art.actions.size());
    for (const auto &action : parts->art.actions)
        names.push_back(action.name);
    return names;
}

void setBakeEnabled(lv_obj_t *root, bool enabled) {
    if (!root) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (parts) parts->bake_enabled = enabled;
}

// —— 滑动方向预取（后台解码）——
// 单飞单份：同一时刻最多一个 worker、一份未消费结果。worker 与 LVGL 线程
// 的握手全部走原子量；解码产物在 worker 本地构建，仅在代际号仍匹配时
// 一次性发布（prefetchCancel 靠递增代际号让在飞结果作废，避免两线程
// 同时碰同一份 vector）。
struct PrefetchJob {
    std::string manifest_json;
    std::string base_dir;
    uint32_t generation = 0;
};

struct PrefetchState {
    std::atomic<bool> in_flight{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<uint32_t> generation{0};
    std::string base_dir;            // 与 specs 同批发布（GIF 动作按它读文件）
    MotionSpec motion;               // worker 发布后只由 LVGL 线程读写
    std::vector<LayerSpec> specs;    // 同上
    std::vector<ActionSpec> actions; // 同上（动作表无堆像素，纯元数据）
};
PrefetchState g_prefetch;

void prefetchWorker(void *param) {
    auto *job = static_cast<PrefetchJob *>(param);
    MotionSpec motion;
    std::vector<LayerSpec> specs;
    std::vector<ActionSpec> actions;
    const bool ok = buildSpecsFromManifest(job->manifest_json, job->base_dir, motion, specs, actions);
    const bool stale = g_prefetch.generation.load(std::memory_order_acquire) != job->generation;
    if (stale) {
        // 预取目标已易主（方向反转/松手取消）：结果就地丢弃。
        for (auto &spec : specs) freeFrames(spec.frames);
    } else if (ok) {
        g_prefetch.base_dir = job->base_dir;
        g_prefetch.motion = motion;
        g_prefetch.specs = std::move(specs);
        g_prefetch.actions = std::move(actions);
        g_prefetch.done.store(true, std::memory_order_release);
        ESP_LOGI("made_sprite", "[sprite] prefetch_ready layers=%u psram_free=%u",
            static_cast<unsigned>(g_prefetch.specs.size()),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    } else {
        for (auto &spec : specs) freeFrames(spec.frames);
        g_prefetch.failed.store(true, std::memory_order_release);
        // 优雅降级：上层回退「先撤旧再解码」老路，这里的失败不致命。
        ESP_LOGW("made_sprite", "[sprite] prefetch_failed psram_free=%u largest=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    }
    g_prefetch.in_flight.store(false, std::memory_order_release);
    delete job;
    vTaskDelete(nullptr);
}

bool prefetchBegin(const std::string &manifest_json, const std::string &base_dir) {
    if (g_prefetch.in_flight.load(std::memory_order_acquire)) return false;
    if (g_prefetch.done.load(std::memory_order_acquire) ||
        g_prefetch.failed.load(std::memory_order_acquire)) return false;
    // 粗粒度守卫：空闲 PSRAM 明显容不下第二套解码像素（一套通常 2~3MB）
    // 就不白费解码——失败本就是优雅降级，这里只是省掉注定失败的 CPU。
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < static_cast<size_t>(1536) * 1024) return false;
    auto *job = new (std::nothrow) PrefetchJob{
        manifest_json, base_dir, g_prefetch.generation.load(std::memory_order_relaxed) + 1};
    if (!job) return false;
    g_prefetch.generation.store(job->generation, std::memory_order_release);
    g_prefetch.in_flight.store(true, std::memory_order_release);
    // libpng 解码栈需求比重采样大：12KB 稳妥。
    if (xTaskCreate(prefetchWorker, "sprite_prefetch", 12288, job, 1, nullptr) != pdPASS) {
        g_prefetch.in_flight.store(false, std::memory_order_release);
        delete job;
        return false;
    }
    return true;
}

bool prefetchReady() {
    return g_prefetch.done.load(std::memory_order_acquire) &&
           !g_prefetch.failed.load(std::memory_order_acquire);
}

bool prefetchInFlight() {
    return g_prefetch.in_flight.load(std::memory_order_acquire);
}

void prefetchCancel() {
    // 在飞的 worker 结束时会发现代际号不匹配、自行丢弃结果；
    // 这里只清已经发布未消费的暂存。
    g_prefetch.generation.fetch_add(1, std::memory_order_release);
    for (auto &spec : g_prefetch.specs) freeFrames(spec.frames);
    g_prefetch.specs.clear();
    g_prefetch.done.store(false, std::memory_order_relaxed);
    g_prefetch.failed.store(false, std::memory_order_relaxed);
}

bool commitPrefetched(lv_obj_t *root) {
    if (!root || g_prefetch.in_flight.load(std::memory_order_acquire) ||
        !prefetchReady()) return false;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->root) return false;
    std::vector<LayerSpec> specs = std::move(g_prefetch.specs);
    std::vector<ActionSpec> actions = std::move(g_prefetch.actions);
    g_prefetch.specs.clear();
    g_prefetch.actions.clear();
    g_prefetch.done.store(false, std::memory_order_relaxed);
    // commitArt 只在成功时撤旧素材（旧容器+旧帧整体退场，不闪空白）；
    // 失败时旧素材原样保留，暂存帧就地释放，上层回退老路。
    if (commitArt(parts, g_prefetch.motion, std::move(specs), std::move(actions), false,
                  g_prefetch.base_dir)) {
        ESP_LOGI("made_sprite", "[sprite] prefetch_committed layers=%u",
            static_cast<unsigned>(parts->art.layers.size()));
        return true;
    }
    for (auto &spec : specs) freeFrames(spec.frames);
    return false;
}

} // namespace made_sprite
