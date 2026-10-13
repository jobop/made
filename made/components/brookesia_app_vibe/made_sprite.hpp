// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include "lvgl.h"

namespace made_sprite {

// 助手页的近全屏小精灵。内置绿豆是「一份编进固件的普通分层素材」
// （img_sprite_bean.c + img_sprite_bean.h 的坐标，SPRITES 格式的第一个实例），
// 与 agent 插件包下发的分层素材（docs/SPRITES.md）走同一条提交与驱动管线；
// 固件没有任何写死的五官坐标——眨眼/说话都是特征层切帧。
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
// 解码在 LVGL 线程进行（数百毫秒级，一次性）；失败返回 false 并保持现状。
// 成功后内置绿豆被替换，动效由 update() 按素材层的 blink/talk/alert 驱动。
bool applyArtwork(lv_obj_t *root, const std::string &manifest_json, const std::string &base_dir);

// 回到内置绿豆并释放下发素材的位图（PSRAM）。
void clearArtwork(lv_obj_t *root);

// 当前是否处于分层素材模式。
bool artworkActive(lv_obj_t *root);

// 内置绿豆换色（0 = 默认绿色）。左右滑切助手时每个助手一种豆色；
// 只对内置绿豆生效，助手下发的分层素材忽略。
void setTint(lv_obj_t *root, uint32_t color);

} // namespace made_sprite
