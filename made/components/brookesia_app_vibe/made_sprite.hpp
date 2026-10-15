// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
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
    kOffline,         // 断连：静止
};
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

// 撤下素材并释放位图（PSRAM）。舞台回到空白。
void clearArtwork(lv_obj_t *root);

// PNG 字节（内存）解码为 ARGB8888（LVGL COLOR_FORMAT_ARGB8888 字节序与
// PNG BGRA 一致）。失败返回 nullptr。调用方负责 heap_caps_free。
uint8_t *decodePngArgbMem(const uint8_t *data, size_t size, int &width, int &height);

// 当前是否处于分层素材模式。
bool artworkActive(lv_obj_t *root);

} // namespace made_sprite
