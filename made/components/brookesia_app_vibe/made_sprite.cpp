// SPDX-License-Identifier: Apache-2.0
#include "made_sprite.hpp"
#include "made_gif.hpp"
#include "vibe_theme.hpp"
#include "made_layout.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
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

void frameFree(uint8_t *p);   // 帧内存统一释放口（池槽/堆），定义见下方帧槽池一节

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

// 整帧状态动画的一条轨道：素材声明「这个状态播这几帧」，固件不猜素材结构。
// first/count 指向合并层（frames 模式下所有状态的帧拼成一个层）的帧区间。
struct StateTrack {
    std::string name;            // idle / talking / blink / success ...
    int first = 0;               // 区间起点（帧下标）
    int count = 0;               // 帧数
    uint32_t frame_ms = 180;     // 每帧时长
    bool periodic = false;       // true = 周期性插入（眨眼），否则按情绪常驻
    uint32_t period_ms = 4000;   // 周期
    uint32_t hold_ms = 120;      // 每次插入持续
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

// 整帧状态动画：素材在 manifest 里声明「每个状态播哪几帧、多快、眨眼怎么
// 插」，固件只照表播。固件不推断素材结构（不猜层的尺寸/是不是特征块），
// 以后新增素材形态也不需要改固件。
struct FrameMode {
    bool enabled = false;
    std::vector<StateTrack> tracks;
    int8_t mood_track[Mood::kMoodCount] = {};   // -1 = 回落到 idle 轨道
    int blink_track = -1;
};

struct Parts {
    lv_obj_t *root = nullptr;      // 舞台根对象
    bool builtin = false;          // 兼容保留：内置素材已删除，恒为 false
    bool bake_enabled = true;      // 侧舞台关烘焙：省 PSRAM 给预取解码
    int last_mood = -1;
    uint32_t last_anim_ms = 0;     // 上次动效驱动的 elapsed（10Hz 节流）
    struct Art {
        lv_obj_t *container = nullptr;
        std::vector<ArtLayer> layers;
        // 整帧状态动画的播放表（素材声明，固件照播）。老素材没有这张表，
        // enabled=false，走原来的分层叠加模型，行为与改动前完全一致。
        FrameMode frame_mode;
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
        // 两级绑定模型：抽象命令号 → 动作名（"A1" → "fall"）。素材作者在
        // manifest 的 commands 表里定义「命令绑素材」；事件绑定（增强插件）
        // 只发命令号，不发语义名。槽位 A1–A9，纯编号不定语义。声明了本表
        // 的素材只认命令号（语义名直发废弃）；没声明的老素材保持语义名直发。
        std::vector<std::pair<std::string, std::string>> commands;
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
                    if (frame.pixels) frameFree(frame.pixels);
                for (auto &frame : layer.baked_frames)
                    if (frame.pixels) frameFree(frame.pixels);
            }
        }
    } art;
};

void freeFrames(std::vector<Frame> &frames) {
    for (auto &frame : frames)
        if (frame.pixels) frameFree(frame.pixels);
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
    FrameMode frame_mode;
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

// shape() 已随任务状态光环（glow）一并移除：角色改小居中后满屏圆盘
// 露在角色身后形似圆框，状态提示改由素材页顶部状态图标承担（2026-10-20）。

int wave(uint32_t elapsed_ms, uint32_t period, int amplitude, int phase = 0) {
    const int angle = static_cast<int>((elapsed_ms % period) * 360 / period);
    return lv_trigo_sin((angle + phase) % 360) * amplitude / LV_TRIGO_SIN_MAX;
}

// 最近一次 applyArtwork 失败原因。prefetchWorker 也会写（同样用 atomic），
// 读方只在 LVGL 线程 applyArtwork 返回 false 后立刻读，偶发竞态只影响
// alloc/decode 归因精度，后果可忽略（见 tryApplyCachedArtwork）。
// 归因必须线程私有：LVGL 线程（tryApplyCachedArtwork）与 worker 线程
// （syncArtwork）可能并发各跑一次 applyArtwork，共享 atomic 会被对方线程
// 的「入口重置为 none」覆盖——alloc 失败被误读成 none，上层判成缓存损坏
// 走 purge+redownload（实机 2026-10-19 晨：PSRAM 紧张时 alloc 失败连打
// purge，越清越糟）。写方（decodePngArgb/applyArtwork）与读方总在同一
// 线程，thread_local 语义正好。
thread_local ApplyFail t_last_apply_fail = ApplyFail::none;

// —— 帧槽池 v2：双尺寸分级 ——
// 帧缓冲直接驻留在 decodePngArgb 的分配里。变长帧分配与长驻帧在堆里交错
// 会把 PSRAM 切碎（2026-10-19 压测实锤：稳态 free≈1.3MB 但最大连续块仅
// 496KB，360 画布单帧 518KB 解码必败，png_alloc_failed 337 次）。ESP-IDF
// 堆不支持搬移式碎片整理，唯一可行机制是让碎片不产生：等尺寸槽从池里
// 取还，堆里只剩池本身这几次启动期分配。
// v1 教训（soak2 实测）：单一 225KB 槽对小帧是灾难——codex 一套 14 帧里
// 13 帧是 bbox 特征帧（eyes 解码 33KB / mouth 3KB），每帧也占一个 225KB
// 槽，3.15MB 只装了 0.5MB 有效像素，池被小帧抽干后 body 解码只能堆回退，
// 耗尽 → wm=67KB、16 分钟 1345 次 alloc 失败。
// v2：大槽（body/全画布帧）+ 小槽（bbox 特征帧）两档；扩池带余量守卫
//（自由 PSRAM 低于阈值不再扩），池尽仍回退堆（旧 360 素材缓存也走堆）。
constexpr size_t kSlotBigBytes = 240 * 240 * 3;   // 168KB：body/全画布帧（RGB565A8 解码目标 = 3 字节/像素）
constexpr int kSlotBigMax = 10;                   // 大槽上限 ≤1.68MB（最坏三套驻留 8 + 未来加助手/加帧余量 2）
constexpr size_t kSlotSmallBytes = 48 * 1024;     // 48KB：bbox 特征帧（实测 eyes 33KB）
constexpr int kSlotSmallMax = 20;                 // 小槽上限 ≤0.96MB（实测 codex 全套 11 小帧 + 余量）
constexpr size_t kPoolReserveBytes = 600 * 1024;  // 扩池前必须留出的自由余量

std::mutex g_slot_mutex;

struct SlotPool {
    size_t bytes;
    int max;
    int count = 0;
    // 硬容量上限必须 ≥ kSlotSmallMax（20）——数组按最大档位留足。
    uint8_t *base[24] = {};
    bool used[24] = {};
};

SlotPool g_pool_big{kSlotBigBytes, kSlotBigMax};
SlotPool g_pool_small{kSlotSmallBytes, kSlotSmallMax};

// 取一个空闲槽；池空且未达上限、且自由余量足够时增长一个新槽；失败返回
// nullptr（调用方回退更大槽或堆分配）。
uint8_t *slotTake(SlotPool &p) {
    std::lock_guard<std::mutex> lock(g_slot_mutex);
    for (int i = 0; i < p.count; ++i) {
        if (!p.used[i]) {
            p.used[i] = true;
            return p.base[i];
        }
    }
    if (p.count < p.max &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > kPoolReserveBytes + p.bytes) {
        auto *q = static_cast<uint8_t *>(
            heap_caps_malloc(p.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (q) {
            p.base[p.count] = q;
            p.used[p.count] = true;
            ++p.count;
            return q;
        }
    }
    return nullptr;
}

// 帧内存统一分配口：≤小槽走小槽（尽可借大槽），≤大槽走大槽，更大或池
// 尽回退堆。与 frameFree 配对（帧可能来自两档池或堆，释放无需区分来源）。
// 烘焙帧（resampleArgb 输出）也走这里——它们与解码帧同样长寿，若留在
// 堆里就是残留的碎片源（每次换助手 free+alloc 12 个变长块）。
uint8_t *frameAlloc(size_t bytes) {
    if (bytes <= kSlotBigBytes) {
        if (bytes <= kSlotSmallBytes) {
            if (uint8_t *p = slotTake(g_pool_small)) return p;
        }
        if (uint8_t *p = slotTake(g_pool_big)) return p;
    }
    return static_cast<uint8_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

// 所有权检查（不释放）：诊断日志用 pooled= 归因帧是否来自池。
bool frameSlotOwnedAny(uint8_t *p) {
    if (!p) return false;
    std::lock_guard<std::mutex> lock(g_slot_mutex);
    for (SlotPool *pool : {&g_pool_big, &g_pool_small}) {
        for (int i = 0; i < pool->count; ++i)
            if (pool->base[i] == p) return true;
    }
    return false;
}

// 帧内存统一释放口：池指针还槽，堆指针 free。帧可能来自两档池或堆，
// freeFrames/Art 析构无需区分来源。
void frameFree(uint8_t *p) {
    if (!p) return;
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(g_slot_mutex);
        for (SlotPool *pool : {&g_pool_big, &g_pool_small}) {
            for (int i = 0; i < pool->count; ++i) {
                if (pool->base[i] == p) {
                    pool->used[i] = false;
                    owned = true;
                    break;
                }
            }
            if (owned) break;
        }
    }
    if (!owned) heap_caps_free(p);
}

// PNG → RGB565A8。LVGL 语义（v9）：RGB565 平面在前（stride = w*2），其后
// 紧跟 A8 alpha 平面（行距 = stride/2 = w），data_size = w*h*3。直出设备
// 主色深（RGB565），帧内存从 ARGB8888 的 4B/px 降到 3B/px（-25%），渲染
// 侧还省一次格式转换。逐行解码：峰值内存就是成品帧本身，libpng 不再持有
// 整幅中间图。16-bit/灰度/调色板/tRNS 通道统一展开成 RGBA8 再打包；
// interlace 逐 pass 重写同一条扫描行（后写胜出，最终数据完整）。
uint8_t *decodePngArgb(const std::string &path, int &width, int &height) {
    width = 0;
    height = 0;
    // 预置 decode 归因：任何失败默认内容问题，只有 frameAlloc 失败才改写 alloc。
    t_last_apply_fail = ApplyFail::decode;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) {
        ESP_LOGE("made_sprite", "[sprite] png_begin_failed path=%s msg=open failed", path.c_str());
        return nullptr;
    }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    // buffer/rowbuf 在 setjmp 之后赋值，longjmp 回来要读它们做清理，必须
    // volatile（C 标准：非 volatile 局变量跨 setjmp/longjmp 修改后值不确定）。
    // png/info/fp 在 setjmp 前就已定形且此后不再改，普通变量即可。
    uint8_t * volatile buffer = nullptr;
    uint8_t * volatile rowbuf = nullptr;
    if (!png || !info) {
        ESP_LOGE("made_sprite", "[sprite] png_begin_failed path=%s msg=libpng init failed", path.c_str());
        if (png) png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(fp);
        return nullptr;
    }
    if (setjmp(png_jmpbuf(png))) {
        // libpng 错误长跳转统一到这里。
        ESP_LOGE("made_sprite", "[sprite] png_begin_failed path=%s msg=decode failed", path.c_str());
        if (rowbuf) free(rowbuf);
        if (buffer) frameFree(buffer);
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        return nullptr;
    }
    png_init_io(png, fp);
    png_read_info(png, info);
    const png_uint_32 w = png_get_image_width(png, info);
    const png_uint_32 h = png_get_image_height(png, info);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        ESP_LOGE("made_sprite", "[sprite] png_begin_failed path=%s msg=bad size %ux%u",
            path.c_str(), static_cast<unsigned>(w), static_cast<unsigned>(h));
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        return nullptr;
    }
    const int color_type = png_get_color_type(png, info);
    if (png_get_bit_depth(png, info) == 16) png_set_strip_16(png);
    if (png_get_bit_depth(png, info) < 8 || color_type == PNG_COLOR_TYPE_PALETTE ||
        png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_expand(png);
    png_set_gray_to_rgb(png);
    if (!(color_type & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    const size_t rowbytes = png_get_rowbytes(png, info);
    if (rowbytes < static_cast<size_t>(w) * 4) {
        // 展开变换后理应得到 RGBA8 行；不符说明遇到了没覆盖的 PNG 变体。
        ESP_LOGE("made_sprite", "[sprite] png_begin_failed path=%s msg=unexpected rowbytes %u",
            path.c_str(), static_cast<unsigned>(rowbytes));
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        return nullptr;
    }

    const size_t bytes = static_cast<size_t>(w) * h * 3;
    buffer = frameAlloc(bytes);
    const bool pooled = buffer && frameSlotOwnedAny(buffer);
    if (!buffer) {
        t_last_apply_fail = ApplyFail::alloc;
        ESP_LOGE("made_sprite", "[sprite] png_alloc_failed path=%s bytes=%u psram_free=%u largest=%u pooled=%d",
            path.c_str(), static_cast<unsigned>(bytes),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
            pooled ? 1 : 0);
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        return nullptr;
    }
    rowbuf = static_cast<uint8_t *>(malloc(rowbytes));
    if (!rowbuf) {
        t_last_apply_fail = ApplyFail::alloc;
        ESP_LOGE("made_sprite", "[sprite] png_alloc_failed path=%s msg=row buffer", path.c_str());
        frameFree(buffer);
        buffer = nullptr;
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        return nullptr;
    }

    uint8_t *rgb = buffer;
    uint8_t *a8 = rgb + static_cast<size_t>(w) * h * 2;
    for (int pass = 0; pass < passes; ++pass) {
        for (png_uint_32 y = 0; y < h; ++y) {
            png_read_row(png, rowbuf, nullptr);
            uint8_t *rgb_row = rgb + static_cast<size_t>(y) * w * 2;
            uint8_t *a8_row = a8 + static_cast<size_t>(y) * w;
            const uint8_t *src = rowbuf;
            for (png_uint_32 x = 0; x < w; ++x, src += 4) {
                // RGBA8 → RGB565（小端内存序，与 LVGL u16 一致）+ A8 直存。
                const uint16_t v = static_cast<uint16_t>(
                    ((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
                rgb_row[x * 2] = static_cast<uint8_t>(v & 0xFF);
                rgb_row[x * 2 + 1] = static_cast<uint8_t>(v >> 8);
                a8_row[x] = src[3];
            }
        }
    }
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    free(rowbuf);
    fclose(fp);
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return buffer;
}

// —— 做法 A：呼吸帧轮播烘焙 ——
// body 层的呼吸缩放（约 ±4%）按等标度步进预烘焙成 K 帧显示尺寸的位图
// 位图（源图先裁到内容 bbox 省内存）。运行时逐拍只换 src + 位移补偿，变换
// 域从「每拍重采样整张大图」缩小到「零变换 blit」——滑动/动效卡顿的最大
// 单一来源。烘焙是一次性 CPU（每帧几毫秒），内存按 SPIRAM 预算从 12 帧
// 逐级降到 4 帧，全失败则回退原动态缩放路径。

// 内容 bbox（alpha≥16 视为有内容）。整图透明返回 false。
// px 为 RGB565A8：A8 平面在 RGB 平面（w*h*2 字节）之后，行距 = w。
bool contentBbox(const uint8_t *px, int w, int h, int &bx, int &by, int &bw, int &bh)
{
    const uint8_t *alpha = px + static_cast<size_t>(w) * h * 2;
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = alpha + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            if (row[x] >= 16) {
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

// ARGB8888 → RGB565A8 打包（烘焙输出转设备主色深用）。565 量化的位复制
// 展开（5→8、6→8）保证往返 565↔888 无系统性偏暗。失败返回 null。
uint8_t *convertArgbToRgb565a8(const uint8_t *src, int w, int h)
{
    const size_t n = static_cast<size_t>(w) * h;
    auto *dst = frameAlloc(n * 3);
    if (!dst) return nullptr;
    uint8_t *rgb = dst;
    uint8_t *a8 = dst + n * 2;
    for (size_t i = 0; i < n; ++i) {
        const uint16_t v = static_cast<uint16_t>(
            ((src[i * 4 + 0] & 0xF8) << 8) | ((src[i * 4 + 1] & 0xFC) << 3) | (src[i * 4 + 2] >> 3));
        rgb[i * 2] = static_cast<uint8_t>(v & 0xFF);
        rgb[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
        a8[i] = src[i * 4 + 3];
    }
    return dst;
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
    auto *dst = frameAlloc(static_cast<size_t>(dw) * dh * 4);
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
            const int bytes = dw * dh * 3;   // 烘焙帧驻留为 RGB565A8
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
        // 烘焙帧转 RGB565A8 驻留（ARGB 中间帧立即释放）：长驻帧省 25% PSRAM。
        auto *packed = convertArgbToRgb565a8(px, dw, dh);
        frameFree(px);
        if (!packed) break;
        g_bake.checksums.push_back(fnv1a(packed, static_cast<size_t>(dw) * dh * 3));
        Frame f;
        f.pixels = packed;
        f.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        f.dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
        f.dsc.header.w = static_cast<uint32_t>(dw);
        f.dsc.header.h = static_cast<uint32_t>(dh);
        f.dsc.header.stride = static_cast<uint32_t>(dw) * 2;
        f.dsc.data_size = static_cast<size_t>(dw) * dh * 3;
        f.dsc.data = packed;
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
    // 源帧是 RGB565A8（3B/px），重采样器吃 ARGB8888：拷贝时同步做 565 位
    // 复制展开（5→8、6→8）。注意必须逐行拷：bbox 在整幅源图里每行之间隔着
    // (sw-bw) 像素不连续数据，整块 memcpy 会把行尾拼上下行开头、逐行漂移
    // (sw-bw)*4 字节，渲染成斜向剪切条纹（真机与 sim 同错，校验和一致但
    // 内容本身是错的——「数据校验正确」不等于「拷贝正确」，校验只能证自洽
    // 不能证对）。
    auto *region = static_cast<uint8_t *>(heap_caps_malloc(
        static_cast<size_t>(bw) * bh * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!region) return;
    {
        const uint8_t *src = static_cast<const uint8_t *>(srcf.dsc.data);
        const size_t sstride = srcf.dsc.header.stride
            ? static_cast<size_t>(srcf.dsc.header.stride)
            : static_cast<size_t>(sw) * 2;
        const uint8_t *a8 = src + static_cast<size_t>(sw) * sh * 2;   // 源图 A8 平面
        const size_t astride = sstride / 2;
        for (int y = 0; y < bh; ++y) {
            const uint8_t *srow = src + static_cast<size_t>(by + y) * sstride + static_cast<size_t>(bx) * 2;
            const uint8_t *arow = a8 + static_cast<size_t>(by + y) * astride + bx;
            uint8_t *out = region + static_cast<size_t>(y) * bw * 4;
            for (int x = 0; x < bw; ++x) {
                const uint16_t v = static_cast<uint16_t>(srow[x * 2] | (srow[x * 2 + 1] << 8));
                const uint8_t r = static_cast<uint8_t>((v >> 11) & 0x1F);
                const uint8_t g = static_cast<uint8_t>((v >> 5) & 0x3F);
                const uint8_t b = static_cast<uint8_t>(v & 0x1F);
                out[x * 4 + 0] = static_cast<uint8_t>((r << 3) | (r >> 2));
                out[x * 4 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
                out[x * 4 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
                out[x * 4 + 3] = arow[x];
            }
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
               std::vector<ActionSpec> &&actions,
               std::vector<std::pair<std::string, std::string>> &&commands,
               bool builtin, const std::string &base_dir) {
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
        // 帧按 manifest 原生像素 1:1 落设计画布（不做画布→设计画布的放大：
        // 用户明确要「素材降档后那个小尺寸」，只要居中不要回大）。缩放支点
        // 必须显式设为左上角：lv_image 的 pivot 默认是图片中心，不设的话
        // 每层绕各自中心放大，层间相对位置全部漂移（实机右下角的
        // 「绿色色块」就是被甩出去的嘴层/豆脚）。
        const int base_scale = spec.scale_mul * made_lv_scale();
        lv_image_set_scale(object, base_scale);
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
    parts->art.frame_mode = motion.frame_mode;   // 没声明 frames 表时 enabled=false，走分层模型
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
    parts->art.commands = std::move(commands);   // 命令号→动作映射随素材整体换（两级绑定）
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
        ESP_LOGI("made_sprite", "[art] layer id=%s frames=%u blink=%d talk=%d emotions=%d period=%u closed=%u",
            layer.id.c_str(), static_cast<unsigned>(layer.frames.size()), layer.blink ? 1 : 0,
            layer.talk ? 1 : 0, layer.has_emotions ? 1 : 0,
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
    // 容器只承载摆动/弹跳的增量：层坐标（made_x/made_home_y）已含设计带
    // 原点偏移，容器再走 made_x/made_home_y 会把原点偏移加第二次——矩形
    // 屏上整个精灵被压下 home_oy（40px）不居中（实机方板实锤）。用 made_s
    // 纯增量，方屏行为与旧代码一致（ox/home_oy=0）。
    lv_obj_set_pos(art.container, made_s(sway), made_s(-bounce));

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
    // —— 整帧状态动画模式：素材已声明每个状态播哪几帧，这里照表播放 ——
    // 整只形象只有一个图层，状态切换只是换 src，不存在叠层重影；眨眼是
    // 周期性插入的一条轨道（素材声明周期与时长），播完自动回当前情绪轨道。
    if (art.frame_mode.enabled && !art.layers.empty()) {
        ArtLayer &layer = art.layers[0];
        if (layer.obj && !layer.frames.empty() &&
            !(overridden.size() && overridden[0])) {
            const int total = static_cast<int>(layer.frames.size());
            const auto &fm = art.frame_mode;
            int ti = fm.mood_track[static_cast<size_t>(mood)];
            if (ti < 0 || ti >= static_cast<int>(fm.tracks.size())) ti = 0;
            // 眨眼优先：它是一条短促的周期性轨道，插在当前情绪之上
            if (fm.blink_track >= 0 && fm.blink_track < static_cast<int>(fm.tracks.size())) {
                const StateTrack &bt = fm.tracks[static_cast<size_t>(fm.blink_track)];
                if (bt.count > 0 && (elapsed_ms % bt.period_ms) < bt.hold_ms) ti = fm.blink_track;
            }
            int frame = 0;
            if (ti >= 0 && ti < static_cast<int>(fm.tracks.size())) {
                const StateTrack &tk = fm.tracks[static_cast<size_t>(ti)];
                const uint32_t ms = tk.frame_ms ? tk.frame_ms : 180;
                frame = tk.first + static_cast<int>((elapsed_ms / ms) % static_cast<uint32_t>(tk.count));
            }
            frame = std::min(std::max(frame, 0), total - 1);
            if (frame != layer.last_frame) {
                layer.last_frame = frame;
                lv_image_set_src(layer.obj, &layer.frames[static_cast<size_t>(frame)].dsc);
            }
        }
        return;   // 整帧模式没有分层选帧/显隐，其余逻辑不适用
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
    // 任务状态光环（glow）已移除（2026-10-20）：角色改小居中后满屏圆盘
    // 露在身后形似圆框；状态提示由素材页顶部状态图标承担。
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

    return root;
}

void update(lv_obj_t *root, uint32_t elapsed_ms, int mood) {
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return;
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->art.active) return;
    // 动效 10Hz 节流：渲染泵 20Hz，但呼吸/轻摆/眨眼都是 1.4–4s 的慢波，
    // 10Hz 采样视觉无差；而每拍动效都会重采样全部图层（RGB565A8 软件缩放）
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
                                   std::vector<ActionSpec> &actions,
                                   std::vector<std::pair<std::string, std::string>> &commands) {
    cJSON *manifest = cJSON_Parse(manifest_json.c_str());
    if (!manifest) return false;
    const cJSON *canvas = cJSON_GetObjectItemCaseSensitive(manifest, "canvas");
    const cJSON *canvas_w = cJSON_GetObjectItemCaseSensitive(canvas, "w");
    const cJSON *canvas_h = cJSON_GetObjectItemCaseSensitive(canvas, "h");
    const cJSON *layers = cJSON_GetObjectItemCaseSensitive(manifest, "layers");
    const cJSON *frames_table_root = cJSON_GetObjectItemCaseSensitive(manifest, "frames");
    // 两种素材形态二选一：layers（分层叠加，老素材）或 frames（整帧状态动画，
    // 声明式）。至少要有一张表，否则素材不可用。
    const bool has_layers = cJSON_IsArray(layers);
    const bool has_frames = cJSON_IsObject(frames_table_root);
    if (!cJSON_IsNumber(canvas_w) || !cJSON_IsNumber(canvas_h) ||
        (!has_layers && !has_frames) || canvas_w->valueint <= 0 || canvas_h->valueint <= 0) {
        cJSON_Delete(manifest);
        return false;
    }

    // 画布居中落进设计画布，呼吸/摆动支点 = 设计画布中心（构图中心）。
    // MotionSpec.canvas_* 唯一消费点就是 pivot，直接存设计值。
    motion.canvas_w = made_design;
    motion.canvas_h = made_design;
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
    for (const cJSON *layer = has_layers ? layers->child : nullptr; layer; layer = layer->next) {
        if (!cJSON_IsObject(layer) || pending.size() >= 16) continue;
        const cJSON *z = cJSON_GetObjectItemCaseSensitive(layer, "z");
        pending.push_back({layer, cJSON_IsNumber(z) ? z->valueint : 0});
    }
    std::stable_sort(pending.begin(), pending.end(),
                     [](const Pending &a, const Pending &b) { return a.z < b.z; });

    // —— 整帧状态动画（可选；有这张表就忽略 layers）——
    // "frames": { "<state>": ["a.png", ...]
    //             | { "frames": [...], "ms": 每帧毫秒,
    //                 "period_ms": 周期, "hold_ms": 每次持续 } }
    // state 取 idle/talking/running/waiting/success/sad/offline，外加 blink
    // （周期性插入的一条短轨）。整套形象只有一个图层，状态切换只换 src——
    // 不存在「一层一只鸟」的叠加重影，也不需要固件去猜素材长什么样。
    {
        static const struct { const char *name; int mood; } kMoods[] = {
            {"idle", Mood::kIdle},           {"talking", Mood::kTalking},
            {"running", Mood::kRunning},     {"waiting", Mood::kWaitingConfirm},
            {"success", Mood::kSuccess},     {"sad", Mood::kSad},
            {"offline", Mood::kOffline},
        };
        for (int i = 0; i < Mood::kMoodCount; ++i) motion.frame_mode.mood_track[i] = -1;
        const cJSON *frames_table = cJSON_GetObjectItemCaseSensitive(manifest, "frames");
        if (cJSON_IsObject(frames_table)) {
            FrameMode fm;
            for (int i = 0; i < Mood::kMoodCount; ++i) fm.mood_track[i] = -1;
            const cJSON *dms = cJSON_GetObjectItemCaseSensitive(manifest, "frame_ms");
            const uint32_t default_ms =
                (cJSON_IsNumber(dms) && dms->valueint >= 30 && dms->valueint <= 2000)
                    ? static_cast<uint32_t>(dms->valueint) : 180;
            LayerSpec spec;
            spec.id = "frames";
            spec.x = (made_design - canvas_w->valueint) / 2;
            spec.y = (made_design - canvas_h->valueint) / 2;
            bool ok = true;
            for (const cJSON *st = frames_table->child; st && ok; st = st->next) {
                StateTrack tk;
                if (!st->string) continue;
                tk.name = st->string;
                tk.frame_ms = default_ms;
                std::vector<std::string> files;
                if (cJSON_IsArray(st)) {
                    for (const cJSON *f = st->child; f; f = f->next)
                        if (cJSON_IsString(f) && f->valuestring) files.push_back(f->valuestring);
                } else if (cJSON_IsObject(st)) {
                    const cJSON *fl = cJSON_GetObjectItemCaseSensitive(st, "frames");
                    if (cJSON_IsArray(fl))
                        for (const cJSON *f = fl->child; f; f = f->next)
                            if (cJSON_IsString(f) && f->valuestring) files.push_back(f->valuestring);
                    const cJSON *ms = cJSON_GetObjectItemCaseSensitive(st, "ms");
                    if (cJSON_IsNumber(ms) && ms->valueint >= 30 && ms->valueint <= 2000)
                        tk.frame_ms = static_cast<uint32_t>(ms->valueint);
                    const cJSON *per = cJSON_GetObjectItemCaseSensitive(st, "period_ms");
                    if (cJSON_IsNumber(per) && per->valueint >= 500 && per->valueint <= 60000)
                        tk.period_ms = static_cast<uint32_t>(per->valueint);
                    const cJSON *hold = cJSON_GetObjectItemCaseSensitive(st, "hold_ms");
                    if (cJSON_IsNumber(hold) && hold->valueint >= 30 && hold->valueint <= 2000)
                        tk.hold_ms = static_cast<uint32_t>(hold->valueint);
                } else {
                    continue;
                }
                if (files.empty() || spec.frames.size() + files.size() > 10) { ok = false; break; }
                tk.first = static_cast<int>(spec.frames.size());
                for (const auto &name : files) {
                    if (name.empty() || name.size() > 32 ||
                        name.find_first_of("/\\") != std::string::npos ||
                        name.find_first_not_of(
                            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
                            std::string::npos) { ok = false; break; }
                    int width = 0, height = 0;
                    auto *pixels = decodePngArgb(base_dir + name, width, height);
                    if (!pixels) { ok = false; break; }
                    Frame frame;
                    frame.pixels = pixels;
                    frame.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                    frame.dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
                    frame.dsc.header.w = static_cast<uint32_t>(width);
                    frame.dsc.header.h = static_cast<uint32_t>(height);
                    frame.dsc.header.stride = static_cast<uint32_t>(width) * 2;
                    frame.dsc.data_size = static_cast<size_t>(width) * height * 3;
                    frame.dsc.data = pixels;
                    spec.frames.push_back(frame);
                }
                if (!ok) break;
                tk.count = static_cast<int>(files.size());
                // 登记必须在 push_back(std::move(tk)) 之前：搬走后 tk.name 已空
                const int index = static_cast<int>(fm.tracks.size());
                const std::string tname = tk.name;
                if (tname == "blink") {
                    fm.blink_track = index;
                } else {
                    for (const auto &entry : kMoods)
                        if (tname == entry.name) fm.mood_track[entry.mood] = index;
                }
                fm.tracks.push_back(std::move(tk));
            }
            if (ok && !spec.frames.empty() && fm.mood_track[Mood::kIdle] >= 0) {
                specs.push_back(std::move(spec));
                fm.enabled = true;
                motion.frame_mode = std::move(fm);
                ESP_LOGI("made_sprite", "[art] 整帧状态动画：%u 条轨道 / %u 帧",
                    static_cast<unsigned>(motion.frame_mode.tracks.size()),
                    static_cast<unsigned>(specs[0].frames.size()));
            } else {
                for (auto &f : spec.frames) if (f.pixels) frameFree(f.pixels);
            }
        }
    }

    // 整帧模式已建好唯一图层，不再解析 layers（老素材没有 frames 表，照旧）
    if (!motion.frame_mode.enabled) for (const auto &entry : pending) {
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
            frame.dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
            frame.dsc.header.w = static_cast<uint32_t>(width);
            frame.dsc.header.h = static_cast<uint32_t>(height);
            frame.dsc.header.stride = static_cast<uint32_t>(width) * 2;
            frame.dsc.data_size = static_cast<size_t>(width) * height * 3;
            frame.dsc.data = pixels;
            spec.frames.push_back(frame);
        }
        if (spec.frames.empty() || !cJSON_IsNumber(x) || !cJSON_IsNumber(y)) goto fail;
        // manifest 层坐标是画布像素：画布整体居中落进 360 设计画布，层坐标
        // 加画布左上角的设计偏移（素材降档 240 后画布不再占满设计画布，
        // 固件负责居中；层间相对位置保持 manifest 原样）。
        spec.x = x->valueint + (made_design - canvas_w->valueint) / 2;
        spec.y = y->valueint + (made_design - canvas_h->valueint) / 2;
        specs.push_back(std::move(spec));
    }

    // 内容双轴居中：画布居中≠内容居中（角色在画布里常画得偏下，实测
    // 绿豆在屏上偏下 38px）。按各层首帧不透明像素的并集 bbox 把「角色
    // 内容的中心」对到设计画布中心（设计带在矩形屏上垂直居中、画布居中
    // ⇒ 内容中心即屏幕中心）。逐像素扫一遍首帧，apply 时一次性开销。
    {
        bool have_bbox = false;
        int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;   // 设计像素坐标系
        for (const auto &spec : specs) {
            const Frame &f = spec.frames.front();
            const int fw = static_cast<int>(f.dsc.header.w);
            const int fh = static_cast<int>(f.dsc.header.h);
            // RGB565A8：A8 平面在 RGB 平面（fw*fh*2 字节）之后
            const uint8_t *alpha = static_cast<const uint8_t *>(f.dsc.data) +
                static_cast<size_t>(fw) * fh * 2;
            for (int py = 0; py < fh; ++py) {
                const uint8_t *row = alpha + static_cast<size_t>(py) * fw;
                for (int px = 0; px < fw; ++px) {
                    if (row[px] <= 8) continue;
                    const int dx = spec.x + px;
                    const int dy = spec.y + py;
                    if (!have_bbox) {
                        have_bbox = true;
                        bx0 = bx1 = dx;
                        by0 = by1 = dy;
                    } else {
                        if (dx < bx0) bx0 = dx;
                        if (dy < by0) by0 = dy;
                        if (dx > bx1) bx1 = dx;
                        if (dy > by1) by1 = dy;
                    }
                }
            }
        }
        if (have_bbox) {
            const int shift_x = made_design / 2 - (bx0 + bx1) / 2;
            const int shift_y = made_design / 2 - (by0 + by1) / 2;
            for (auto &spec : specs) {
                spec.x += shift_x;
                spec.y += shift_y;
            }
        }
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

    // —— 命令映射（可选）——
    // "commands": { "A1": "<动作名>" }：抽象命令号 → actions 表动作。事件
    // 绑定（增强插件配置）发命令号，素材作者决定命令号播什么，两级解耦。
    // 槽位固定 A1–A9，纯编号不定语义；坏条目（号不合规/动作未声明）跳过。
    if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(manifest, "commands"))) {
        const cJSON *commands_json = cJSON_GetObjectItemCaseSensitive(manifest, "commands");
        for (const cJSON *cmd = commands_json->child; cmd && commands.size() < 9; cmd = cmd->next) {
            if (!cmd->string || !cJSON_IsString(cmd) || !cmd->valuestring || !cmd->valuestring[0]) continue;
            const std::string slot = cmd->string;
            const std::string target = cmd->valuestring;
            if (slot.size() != 2 || slot[0] != 'A' || slot[1] < '1' || slot[1] > '9') continue;
            bool known = false;
            for (const auto &action : actions)
                if (action.name == target) { known = true; break; }
            if (!known) continue;   // 映射目标必须是 actions 表已声明的动作
            bool dup = false;
            for (const auto &entry : commands)
                if (entry.first == slot) { dup = true; break; }
            if (!dup) commands.emplace_back(slot, target);
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
    t_last_apply_fail = ApplyFail::none;
    if (!root) {
        t_last_apply_fail = ApplyFail::bad_input;
        return false;
    }
    auto *parts = static_cast<Parts *>(lv_obj_get_user_data(root));
    if (!parts || !parts->root) {
        t_last_apply_fail = ApplyFail::bad_input;
        return false;
    }
    // 先快速校验 manifest 合法（完整解析在 buildSpecsFromManifest 里再做，
    // 纯解析开销可忽略）——manifest 不合法时不能撤旧素材。
    cJSON *manifest = cJSON_Parse(manifest_json.c_str());
    if (!manifest) {
        t_last_apply_fail = ApplyFail::bad_input;
        return false;
    }
    const cJSON *canvas = cJSON_GetObjectItemCaseSensitive(manifest, "canvas");
    const cJSON *canvas_w = cJSON_GetObjectItemCaseSensitive(canvas, "w");
    const cJSON *canvas_h = cJSON_GetObjectItemCaseSensitive(canvas, "h");
    const cJSON *layers = cJSON_GetObjectItemCaseSensitive(manifest, "layers");
    // 与 buildSpecsFromManifest 同一套判据：layers（老分层素材）或 frames
    // （整帧状态动画）二选一。这里漏放行 frames 会让整帧素材永远在入口被
    // 拒（bad_input），上层 purge+redownload 也修不好（2026-10-24 雪球
    // 白屏死循环实锤：预取路径直达 buildSpecs 能显示，主台走这里全灭）。
    const cJSON *frames_table = cJSON_GetObjectItemCaseSensitive(manifest, "frames");
    if (!cJSON_IsNumber(canvas_w) || !cJSON_IsNumber(canvas_h) ||
        (!cJSON_IsArray(layers) && !cJSON_IsObject(frames_table)) ||
        canvas_w->valueint <= 0 || canvas_h->valueint <= 0) {
        cJSON_Delete(manifest);
        t_last_apply_fail = ApplyFail::bad_input;
        return false;
    }
    // 最大连续块容不下最大层的解码像素（RGB565A8 = 3B/px）：先丢预取暂存帧腾内存。
    // 预取是邻页的备胎，优先级低于当前页显示；prefetchBegin 有 1.5MB 门槛，
    // 内存紧张期被丢弃后不会立即重占。不腾的话解码 alloc 失败，旧素材已撤，
    // 舞台白屏等重试（实机「划着划着雪球变白」）。
    const size_t layer_bytes =
        static_cast<size_t>(canvas_w->valueint) * canvas_h->valueint * 3;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < layer_bytes) {
        ESP_LOGW("made_sprite",
            "[sprite] alloc_tight need=%u largest=%u -> drop prefetch frames",
            static_cast<unsigned>(layer_bytes),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        prefetchCancel();
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
    std::vector<std::pair<std::string, std::string>> commands;
    if (!buildSpecsFromManifest(manifest_json, base_dir, motion, specs, actions, commands)) {
        // buildSpecs 内部的解码失败已由 decodePngArgb 归因（alloc/decode）。
        // 只有没被归因过的失败（manifest 结构坏、层表空等）才算 bad_input——
        // 无条件覆盖会把 alloc 失败伪装成缓存坏，上层 tryApplyCachedArtwork
        // 计数 2 次后 purge+redownload，而内存没释放照样失败，形成
        // 「删缓存→重下→解码失败→再删」死循环（2026-10-20 实机实锤，
        // 单次会话 purge 15+ 次，把其他助手的缓存与预取也搅乱）。
        if (t_last_apply_fail == ApplyFail::none) {
            t_last_apply_fail = ApplyFail::bad_input;
        }
        return false;
    }

    if (commitArt(parts, motion, std::move(specs), std::move(actions), std::move(commands),
                  false, base_dir)) {
        lv_obj_invalidate(root);
        return true;
    }
    if (t_last_apply_fail == ApplyFail::none) {
        t_last_apply_fail = ApplyFail::decode;
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
        try {
            out.append(buffer, read);
        } catch (...) {
            // 内存紧张降级：返回空让上层走 gif_unavailable，异常逃出线程会 abort。
            fclose(file);
            return {};
        }
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
    // 两级绑定模型：素材声明了命令映射（commands 表）时只认抽象命令号
    // （A1–A9），命中后转成目标动作名再播；未映射的命令号静默降级。
    // 语义名直发废弃——防插件作者绕过命令层与素材隐式耦合。
    // 没声明 commands 的老素材保持语义名直发（向后兼容，文档已标记弃用）。
    std::string target = name;
    if (!art.commands.empty()) {
        bool hit = false;
        for (const auto &entry : art.commands) {
            if (entry.first == name) {
                target = entry.second;
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }
    for (size_t i = 0; i < art.actions.size(); ++i) {
        if (art.actions[i].name != target) continue;
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
    // 两级绑定模型：声明了命令映射的素材上报命令号（action.A1 …），否则
    // 上报动作语义名（老素材兼容，弃用中）。
    if (!parts->art.commands.empty()) {
        names.reserve(parts->art.commands.size());
        for (const auto &entry : parts->art.commands)
            names.push_back(entry.first);
        return names;
    }
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
    std::vector<std::pair<std::string, std::string>> commands; // 命令映射（同上）
};
PrefetchState g_prefetch;

void prefetchWorker(void *param) {
    auto *job = static_cast<PrefetchJob *>(param);
    MotionSpec motion;
    std::vector<LayerSpec> specs;
    std::vector<ActionSpec> actions;
    std::vector<std::pair<std::string, std::string>> commands;
    const bool ok = buildSpecsFromManifest(job->manifest_json, job->base_dir, motion, specs, actions,
                                           commands);
    const bool stale = g_prefetch.generation.load(std::memory_order_acquire) != job->generation;
    if (stale) {
        // 预取目标已易主（方向反转/松手取消）：结果就地丢弃。
        for (auto &spec : specs) freeFrames(spec.frames);
    } else if (ok) {
        g_prefetch.base_dir = job->base_dir;
        g_prefetch.motion = motion;
        g_prefetch.specs = std::move(specs);
        g_prefetch.actions = std::move(actions);
        g_prefetch.commands = std::move(commands);
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
    std::vector<std::pair<std::string, std::string>> commands = std::move(g_prefetch.commands);
    g_prefetch.specs.clear();
    g_prefetch.actions.clear();
    g_prefetch.commands.clear();
    g_prefetch.done.store(false, std::memory_order_relaxed);
    // commitArt 只在成功时撤旧素材（旧容器+旧帧整体退场，不闪空白）；
    // 失败时旧素材原样保留，暂存帧就地释放，上层回退老路。
    if (commitArt(parts, g_prefetch.motion, std::move(specs), std::move(actions),
                  std::move(commands), false, g_prefetch.base_dir)) {
        ESP_LOGI("made_sprite", "[sprite] prefetch_committed layers=%u",
            static_cast<unsigned>(parts->art.layers.size()));
        return true;
    }
    for (auto &spec : specs) freeFrames(spec.frames);
    return false;
}

ApplyFail lastApplyFail() {
    return t_last_apply_fail;
}

} // namespace made_sprite
