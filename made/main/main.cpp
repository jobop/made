/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "boost/thread.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h"
#include "./dark/stylesheet.hpp"
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_wifi.hpp"
#include "vibe_theme.hpp"


using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;
using namespace esp_brookesia::apps;

#define TAG              "main"

constexpr bool EXAMPLE_SHOW_MEM_INFO = true;




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


    ESP_UTILS_LOGI("Display ESP-Brookesia phone demo");
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
    // The complete launcher needs more LVGL stack than the one-app example.
    display_cfg.lv_adapter_cfg.task_stack_size = 20 * 1024;
    display_cfg.lv_adapter_cfg.stack_in_psram = false;
    lv_display_t *disp = bsp_display_start_with_config(&display_cfg);
    lv_indev_t *tp = bsp_display_get_input_dev();
    ESP_UTILS_CHECK_ERROR_EXIT(bsp_display_backlight_on(), "Turn on display backlight failed");
    /* Configure GUI lock */
    LvLock::registerCallbacks([](int timeout_ms) {
        esp_err_t ret = bsp_display_lock(timeout_ms);
        ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, false, "Lock failed (timeout_ms: %d)", timeout_ms);

        return true;
    }, []() {
        bsp_display_unlock();
        return true;
    });

    /* Create a phone object */
    ESP_LOGI(TAG, "Create phone object");
    systems::phone::Phone *phone = new systems::phone::Phone(disp);
    phone->setTouchDevice(tp);
    
    Stylesheet *stylesheet = new systems::phone::Stylesheet(STYLESHEET_360_360_DARK);
    ESP_UTILS_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");

    ESP_UTILS_LOGI("Using stylesheet (%s)", stylesheet->core.name);
    ESP_UTILS_CHECK_FALSE_EXIT(phone->addStylesheet(stylesheet), "Add stylesheet failed");
    ESP_UTILS_CHECK_FALSE_EXIT(phone->activateStylesheet(stylesheet), "Activate stylesheet failed");
    delete stylesheet;

    {
        LvLockGuard gui_guard;

        /* Begin the phone */
        ESP_UTILS_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");

        const int made_app_id = phone->installApp(VibeCoding::requestInstance());
        ESP_UTILS_CHECK_FALSE_EXIT(made_app_id >= 1, "Install Made failed");

        // Use the managed launcher lifecycle on the LVGL task (20 KB stack),
        // not app_main's small stack. Made connects as soon as its screen is up.
        lv_timer_t *boot_made = lv_timer_create([](lv_timer_t *timer) {
            auto *phone = static_cast<Phone *>(lv_timer_get_user_data(timer));
            const systems::base::Context::AppEventData start_made = {
                VibeCoding::requestInstance()->getId(), systems::base::Context::AppEventType::START, nullptr
            };
            ESP_UTILS_CHECK_FALSE_EXIT(phone->sendAppEvent(&start_made), "Start Made failed");
        }, 50, phone);
        ESP_UTILS_CHECK_NULL_EXIT(boot_made, "Create Made boot timer failed");
        lv_timer_set_repeat_count(boot_made, 1);
        lv_timer_set_auto_delete(boot_made, true);
    }

    if constexpr (EXAMPLE_SHOW_MEM_INFO) {
        esp_utils::thread_config_guard thread_config({
            .name = "mem_info",
            .stack_size = 4096,
        });
        boost::thread([ = ]() {
            char buffer[128];    /* Make sure buffer is enough for `sprintf` */
            size_t internal_free = 0;
            size_t internal_total = 0;
            size_t external_free = 0;
            size_t external_total = 0;

            while (1) 
            {

                internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                sprintf(buffer,
                        "\t           Biggest /     Free /    Total\n"
                        "\t  SRAM : [%8d / %8d / %8d]\n"
                        "\t PSRAM : [%8d / %8d / %8d]",
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), internal_free, internal_total,
                        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), external_free, external_total);
                //ESP_UTILS_LOGI("\n%s", buffer);
                
                {
                    LvLockGuard gui_guard;
                    ESP_UTILS_CHECK_FALSE_EXIT(
                        phone->getDisplay().getRecentsScreen()->setMemoryLabel(
                            internal_free / 1024, internal_total / 1024, external_free / 1024, external_total / 1024
                        ), "Set memory label failed"
                    );
                }

                boost::this_thread::sleep_for(boost::chrono::seconds(2));
            }
        }).detach();
    }

}
