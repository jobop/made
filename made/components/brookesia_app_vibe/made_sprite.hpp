// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "lvgl.h"

namespace made_sprite {

// 助手页的近全屏小精灵。与 agent 插件包下发的分层素材（docs/SPRITES.md）
// 走同一条提交与驱动管线；固件没有任何写死的五官坐标——眨眼/说话都是
// 特征层切帧。内置绿豆已删除：素材缺失时舞台留空。
namespace Mood {
enum Value {
    kIdle,            // 待机：呼吸 + 轻摆 + 定时眨眼（眨眼待分层素材）
    kTalking,         // 录音/说话：轻快跳动
    kRunning,         // 任务运行中：呼吸加快
    kWaitingConfirm,  // 任务待确认：身后光圈脉冲提醒（BOOT 双击确认/三击取消）
    kSuccess,         // 任务刚完成（瞬时 ~3.5s）：素材有 emotions.success 时换笑脸
    kSad,             // 任务刚失败/取消（瞬时 ~3.5s）：素材有 emotions.sad 时换哭脸
    kOffline,         // 断连：静止
};
// 情绪表（manifest 的 emotions）按下标索引，新增情绪追加在 kOffline 之前。
constexpr int kMoodCount = kOffline + 1;
}

// LVGL-thread only. Returns a full-screen, non-scrollable sprite stage.
// Deleting root releases private state; no independent animation timers exist.
lv_obj_t *create(lv_obj_t *parent);

// elapsed_ms is time since the stage became visible. Call at ~50 ms from render().
void update(lv_obj_t *root, uint32_t elapsed_ms, int mood);

// 分层素材模式（docs/SPRITES.md）。manifest_json 是桥接器 sprite 端点下发的
// manifest.json 原文；base_dir 是素材 PNG 所在目录（含尾部斜杠可省）。
// 解码在 LVGL 线程进行（数百毫秒级，一次性）；失败返回 false（旧素材已先撤，
// 舞台留空）。成功后动效由 update() 按素材层的 blink/talk/alert 驱动。
bool applyArtwork(lv_obj_t *root, const std::string &manifest_json, const std::string &base_dir);

// 最近一次 applyArtwork 失败的原因。alloc = PSRAM 不足/碎片化（缓存文件
// 未必坏，调用方不应据此判死缓存 purge 重下）；decode/bad_input = 内容问题。
enum class ApplyFail { none, bad_input, decode, alloc };
ApplyFail lastApplyFail();

// 撤下素材并释放位图（PSRAM）。舞台回到空白。
void clearArtwork(lv_obj_t *root);

// —— 滑动方向预取（后台解码，松手零等待换装）——
// 滑动一开始就在后台任务里把目标助手的 PNG 全部解码成 RGB565A8 像素暂存
// （只占 PSRAM，不建 LVGL 对象，屏幕上仍只有当前一套在渲染）；松手裁决
// 出目标后 commitPrefetched 原子换装（旧素材存活到提交成功那一刻才释放，
// 不闪空白）。PSRAM 不足以同时容纳两套时解码会失败——那是优雅降级：
// 放弃预取，回退 applyArtwork 的「先撤旧再解码」老路，最差等于现状。
//
// prefetchBegin 在 LVGL 线程调用：解析交给后台任务，立即返回；已有预取
// 在飞或已有未消费结果时返回 false（先 prefetchCancel 再重试）。
// commitPrefetched 必须在 LVGL 线程调用；成功消费暂存并返回 true。
bool prefetchBegin(const std::string &manifest_json, const std::string &base_dir);
bool prefetchReady();       // 后台解码完成，随时可 commitPrefetched
bool prefetchInFlight();    // 后台解码进行中
void prefetchCancel();      // 丢弃未消费的结果；在飞的解码结束时自行丢弃
bool commitPrefetched(lv_obj_t *root);

// PNG 字节（内存）解码为 ARGB8888（LVGL COLOR_FORMAT_ARGB8888 字节序与
// PNG BGRA 一致）。失败返回 nullptr。调用方负责 heap_caps_free。
uint8_t *decodePngArgbMem(const uint8_t *data, size_t size, int &width, int &height);

// 当前是否处于分层素材模式。
bool artworkActive(lv_obj_t *root);

// —— 行动系统（manifest 的 actions 表 + commands 命令映射）——
// 动作 = 对既有层的帧序覆盖（tracks: 层id → 帧序号序列），播完自动回到
// 情绪/状态驱动。支持哪些动作由素材包决定：当前助手的 manifest 没声明、
// 或声明的动作引用了不存在的层/帧，这个动作就不可用。
//
// 两级绑定模型（docs/SPRITES.md）：素材 manifest 的 commands 表（可选，
// "A1"–"A9" → 动作名）定义「命令绑素材」。声明了 commands 的素材只认抽象
// 命令号（playAction("A1")），语义名直发废弃；未声明的老素材保持语义名
// 直发（弃用中，向后兼容）。事件绑定（增强插件）只发命令号。
//
// playAction 必须在 LVGL 线程调用；未知动作/素材缺失时返回 false 且无任何
// 副作用（静默降级，调用方无需处理）。再次调用会打断上一个动作。
bool playAction(lv_obj_t *root, const char *name);

// 当前素材对外可触发的命令名单（LVGL 线程调用；应用层拿它向桥接器上报）：
// 声明了 commands 映射的素材返回命令号（A1–A9），否则返回动作语义名。
std::vector<std::string> actionNames(lv_obj_t *root);

// 开关该舞台的后台呼吸烘焙（默认开）。侧舞台应关闭：烘焙是内存换 CPU
// 的买卖（原始帧被释放、换成多帧 bbox 位图，占用往往更大），侧舞台用
// 动态缩放动画即可，把 PSRAM 留给下一屏的预取解码。
void setBakeEnabled(lv_obj_t *root, bool enabled);

} // namespace made_sprite
