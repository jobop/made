// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include "lvgl.h"

namespace made_sprite {

// 助手页的近全屏小精灵。程序化绘制的占位形象（豆豆身体 + 四肢 + 舞台光斑），
// 后续由 agent 插件包下发的分层素材（docs/SPRITES.md）替换或叠加。
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

} // namespace made_sprite
