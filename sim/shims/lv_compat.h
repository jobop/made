// sim/shims/lv_compat.h — host 兼容垫片（仅模拟器编译时 -include 注入）。
// 设备侧工具链（xtensa clang, gnu++2b, 宽松模式）允许 lv_event_get_target()
// 返回的 void* 隐式转成 lv_obj_t*；Apple clang 不允许。固件源码不动，
// 这里补等价重载，行为与设备一致。
#pragma once

#include "lvgl.h"

inline void *lv_obj_get_user_data(void *obj) { return lv_obj_get_user_data((lv_obj_t *)obj); }
inline int32_t lv_obj_get_index(const void *obj) { return lv_obj_get_index((const lv_obj_t *)obj); }
