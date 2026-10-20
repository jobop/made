// SPDX-License-Identifier: Apache-2.0
// 截图模块（debug.screenshot 命令的执行体）：LVGL 线程内渲染当前画面到
// PSRAM 自建 draw buf，随后开独立任务以 RGB565 裸帧 POST /device/screenshot，
// 桥接器转 PNG 落盘。
//
// 为什么不用 lv_snapshot_create_draw_buf（历史踩坑记录）：
//   1. lv_malloc 只走内部堆（sdkconfig 未开 SPIRAM_USE_MALLOC，free 仅
//      ~130KB），而 screen 的 ext_draw_size=屏宽会把快照区撑到 ~1.9MB——
//      分配必挂且静默返回，截图功能真机恒哑（无任何报错、无 POST）。
//   2. 修法：临时清零 ext_draw_size 按可视区截图，draw buf 用 heap_caps
//      直接在 PSRAM 自建（lv_draw_buf_init），拍完恢复 ext。上传完手动
//      heap_caps_free，不能走 lv_draw_buf_destroy（会用 lv_free 释放 PSRAM）。
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_ui_shared.hpp"

#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace esp_brookesia::apps {

void VibeCoding::startScreenshotUpload()
{
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (screenshot_busy_.exchange(true)) {
        // 已有截屏在跑：刷新“卡死看门狗”起点，避免误伤正常进行的上传。
        screenshot_busy_since_ms_.store(now_ms);
        return;
    }
    screenshot_busy_since_ms_.store(now_ms);
    lv_obj_t *screen = lv_screen_active();
    if (!screen) {
        screenshot_busy_.store(false);
        return;
    }
    lv_obj_update_layout(screen);

    // 临时清零 ext_draw_size：快照区收敛到可视屏（~412KB），侧舞台屏外区
    // 不进快照（截图本就只关心看得见的画面）。拍完恢复。
    const int32_t ext_saved = lv_obj_get_ext_draw_size(screen);
    lv_obj_set_ext_draw_size(screen, 0);
    lv_obj_refresh_ext_draw_size(screen);
    lv_obj_update_layout(screen);

    const int32_t width = lv_obj_get_width(screen);
    const int32_t height = lv_obj_get_height(screen);
    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(height) + LV_DRAW_BUF_ALIGN;
    uint8_t *raw = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    lv_draw_buf_t *buffer = raw ? static_cast<lv_draw_buf_t *>(
        heap_caps_malloc(sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
    lv_result_t shot = LV_RESULT_INVALID;
    if (raw && buffer) {
        if (lv_draw_buf_init(buffer, width, height, LV_COLOR_FORMAT_RGB565, stride, raw,
                             static_cast<uint32_t>(bytes)) == LV_RESULT_OK) {
            lv_draw_buf_set_flag(buffer, LV_IMAGE_FLAGS_MODIFIABLE);
            shot = lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, buffer);
        }
    } else {
        ESP_LOGW(BUTTON_TAG, "[shot] alloc_failed psram_free=%u bytes=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(bytes));
    }

    lv_obj_set_ext_draw_size(screen, ext_saved);
    lv_obj_refresh_ext_draw_size(screen);

    if (shot != LV_RESULT_OK) {
        if (raw) heap_caps_free(raw);
        if (buffer) heap_caps_free(buffer);
        screenshot_busy_.store(false);
        return;
    }

    struct ShotArgs {
        VibeCoding *self;
        uint8_t *data;
        size_t bytes;
        lv_draw_buf_t *buf;
        int width;
        int height;
        uint32_t stride;
    };
    if (xTaskCreate([](void *arg) {
            auto *args = static_cast<ShotArgs *>(arg);
            char path[96];
            snprintf(path, sizeof(path), "/device/screenshot?fmt=rgb565&w=%d&h=%d&stride=%lu",
                     args->width, args->height, static_cast<unsigned long>(args->stride));
            std::string response;
            int status = 0;
            // 裸指针零拷贝上传：412KB 塞进 std::string 会在内部堆上炸。
            (void)args->self->requestRaw(path, true, "application/octet-stream",
                                         args->data, args->bytes, response, status, 4096, 20000);
            heap_caps_free(args->data);
            heap_caps_free(args->buf);
            args->self->screenshot_busy_.store(false);
            delete args;
            vTaskDelete(nullptr);
        }, "vibe_shot", 16384, new ShotArgs{this, raw, static_cast<size_t>(stride) * static_cast<size_t>(height),
                                            buffer, width, height, stride}, 3, nullptr) != pdPASS) {
        heap_caps_free(raw);
        heap_caps_free(buffer);
        screenshot_busy_.store(false);
    }
}

} // namespace esp_brookesia::apps
