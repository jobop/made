/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h"
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_wifi.hpp"
#include "vibe_theme.hpp"


using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::apps;

#define TAG              "main"




extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(vibe_wifi::start());
    const esp_err_t spiffs_result = bsp_spiffs_mount();
    if (spiffs_result != ESP_OK) {
        ESP_LOGW(TAG, "Optional font assets unavailable: %s", esp_err_to_name(spiffs_result));
    }

    if (vibe_theme::init() != ESP_OK) {
        ESP_LOGW(TAG, "Theme init failed; using default palette");
    }


    ESP_UTILS_LOGI("Start Made");
    bsp_display_cfg_t display_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    display_cfg.lv_adapter_cfg.task_stack_size = 20 * 1024;
    display_cfg.lv_adapter_cfg.stack_in_psram = false;
    lv_display_t *disp = bsp_display_start_with_config(&display_cfg);
    ESP_UTILS_CHECK_NULL_EXIT(disp, "Start display failed");
    /* Configure GUI lock */
    LvLock::registerCallbacks([](int timeout_ms) {
        esp_err_t ret = bsp_display_lock(timeout_ms);
        ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, false, "Lock failed (timeout_ms: %d)", timeout_ms);

        return true;
    }, []() {
        bsp_display_unlock();
        return true;
    });

    {
        LvLockGuard gui_guard;
        // Draw Made on the LVGL task. app_main's stack is too small for the UI.
        lv_timer_t *boot_made = lv_timer_create([](lv_timer_t *) {
            ESP_UTILS_CHECK_FALSE_EXIT(VibeCoding::startOnDisplay(), "Start Made failed");
            lv_refr_now(lv_display_get_default());
            bsp_display_backlight_on();
        }, 50, nullptr);
        ESP_UTILS_CHECK_NULL_EXIT(boot_made, "Create Made boot timer failed");
        lv_timer_set_repeat_count(boot_made, 1);
        lv_timer_set_auto_delete(boot_made, true);
    }
}
