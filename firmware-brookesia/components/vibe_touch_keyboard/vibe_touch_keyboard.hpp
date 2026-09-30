// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lvgl.h"

namespace vibe_touch_keyboard {

// Five wide keys per row, sized for a 360 px touch display. The last key
// sends LV_EVENT_READY to the target textarea.
// numeric keeps the keyboard strictly numeric (for ports). number_first
// starts with digits but offers ABC and all text pages (for receiver passwords).
lv_obj_t *create(lv_obj_t *parent, lv_obj_t *target,
                 int x, int y, int width, int height, bool numeric = false,
                 bool number_first = false);

} // namespace vibe_touch_keyboard
