// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include "lvgl.h"

namespace made_lock_screen {

// LVGL-thread only. Returns a full 360×360, non-scrollable gesture surface.
// The callback receives GESTURE, PRESSED, RELEASED and PRESS_LOST on root,
// with the supplied user_data, supporting both quick swipes and slow drags.
// Deleting root releases private state; no independent animation timers exist.
lv_obj_t *create(lv_obj_t *parent, lv_event_cb_t swipe_callback, void *user_data);

// elapsed_ms is time since this lock screen became visible. Call at ~50 ms.
// The first slogan follows english, then Chinese/English alternate every 5 s.
// The swipe hint always follows english. Caller hides root after unlocking.
void update(lv_obj_t *root, uint32_t elapsed_ms, bool english);

} // namespace made_lock_screen
