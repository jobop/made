#include "esp_brookesia_app_vibe.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

#include "board_voice_audio.h"
#include "boot_click_gesture.hpp"
#include "made_layout.hpp"
#include "made_sprite.hpp"
#include "made_sprite_cache.hpp"
#include "new_message_chime.hpp"
#include "vibe_motion.hpp"
#include "cJSON.h"
#include "bsp/display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "mbedtls/md.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "vibe_pairing.hpp"
#include "vibe_voice_capture.hpp"
#include "vibe_wifi.hpp"
#include "vibe_theme.hpp"
#include "vibe_usb.hpp"
#include "vibe_i18n.hpp"
#include "vibe_touch_keyboard.hpp"
#include "vibe_phone_setup.hpp"

using esp_brookesia::systems::phone::App;

LV_IMG_DECLARE(img_app_vibe);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_4);
#include "vibe_ui_shared.hpp"

namespace esp_brookesia::apps {

namespace {
// 候选超时（秒）；0 表示不熄屏。
constexpr uint32_t kScreenOffOptions[] = {0, 30, 60, 300};

void loadScreenOffTimeoutImpl(uint32_t &out)
{
    nvs_handle_t handle;
    if (nvs_open("vibe_power", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t stored = 0;
    if (nvs_get_u8(handle, "off_s", &stored) == ESP_OK) out = stored;
    nvs_close(handle);
}

bool saveScreenOffTimeoutImpl(uint32_t value)
{
    nvs_handle_t handle;
    if (nvs_open("vibe_power", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_u8(handle, "off_s", static_cast<uint8_t>(value));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}
} // namespace

void VibeCoding::loadScreenOffTimeout()
{
    loadScreenOffTimeoutImpl(screen_off_timeout_s_);
    // NVS 里存了不认识的值就退回不熄屏。
    bool known = false;
    for (uint32_t option : kScreenOffOptions)
        if (screen_off_timeout_s_ == option) known = true;
    if (!known) screen_off_timeout_s_ = 0;
}

void VibeCoding::renderPower()
{
    if (!power_timeout_label_) return;
    const char *text = screen_off_timeout_s_ == 0   ? T("不熄屏") :
                       screen_off_timeout_s_ == 30  ? "30 s" :
                       screen_off_timeout_s_ == 60  ? "60 s" : "300 s";
    lv_label_set_text(power_timeout_label_, (std::string(T("空闲熄屏：")) + text).c_str());
}

void VibeCoding::showPower(bool visible)
{
    if (!power_panel_) return;
    if (visible) {
        lv_obj_add_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(theme_panel_, LV_OBJ_FLAG_HIDDEN);
        if (firmware_panel_) lv_obj_add_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN);
        shutdown_armed_ = false;
        if (power_shutdown_label_) lv_label_set_text(power_shutdown_label_, T("关机"));
        renderPower();
        lv_obj_remove_flag(power_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(power_panel_);
    } else {
        lv_obj_add_flag(power_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VibeCoding::powerOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showPower(true);
}

void VibeCoding::showFirmware(bool visible)
{
    if (!firmware_panel_) return;
    if (visible) {
        lv_obj_add_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
        if (power_panel_) lv_obj_add_flag(power_panel_, LV_OBJ_FLAG_HIDDEN);
        if (theme_panel_) lv_obj_add_flag(theme_panel_, LV_OBJ_FLAG_HIDDEN);
        firmware_status_drawn_.clear();
        lv_obj_remove_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(firmware_panel_);
        renderFirmware();
    } else {
        lv_obj_add_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VibeCoding::renderFirmware()
{
    if (!firmware_status_label_ || lv_obj_has_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN)) return;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        text = firmware_status_;
    }
    if (text.empty()) text = T("电脑上还没有配置固件地址");
    if (text == firmware_status_drawn_) return;
    firmware_status_drawn_ = text;
    lv_label_set_text(firmware_status_label_, text.c_str());
}

void VibeCoding::firmwareOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showFirmware(true);
}

void VibeCoding::firmwareBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showFirmware(false);
}

void VibeCoding::firmwareUpdateCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || self->firmware_busy_.load() || self->firmware_update_requested_.load()) return;
    std::lock_guard<std::mutex> lock(self->model_mutex_);
    if (!self->firmware_available_.load()) {
        self->firmware_status_ = T("电脑上还没有配置固件地址");
        return;
    }
    self->firmware_status_ = T("正在从配置的地址下载固件…");
    self->firmware_trace_stage_ = "ready";
    self->firmware_update_requested_.store(true);
    ESP_LOGW("vibe_fw", "[fw] tap");
}

void VibeCoding::powerBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showPower(false);
}

void VibeCoding::powerShutdownCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->power_shutdown_label_) return;
    const uint32_t now = lv_tick_get();
    if (!self->shutdown_armed_ || now - self->shutdown_armed_ms_ > 4000) {
        self->shutdown_armed_ = true;
        self->shutdown_armed_ms_ = now;
        lv_label_set_text(self->power_shutdown_label_, T("再点一次关机"));
        return;
    }
    self->shutdownDevice();
}

void VibeCoding::shutdownDevice()
{
    if (power_shutdown_label_) lv_label_set_text(power_shutdown_label_, T("正在关机"));
    lv_refr_now(nullptr);
    (void)bsp_display_backlight_off();
    // BOOT 是按住说话，不能拿来开机。触摸中断脚平时被上拉为高，
    // 点屏幕时被拉低。等手指离开后再睡，避免这一下立刻把机器唤醒。
#if CONFIG_BSP_BOARD_LCD_2_8
    constexpr gpio_num_t wake_pin = GPIO_NUM_17;
#else
    constexpr gpio_num_t wake_pin = GPIO_NUM_4;
#endif
    for (int i = 0; i < 100 && gpio_get_level(wake_pin) == 0; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(40));
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (rtc_gpio_init(wake_pin) != ESP_OK ||
        rtc_gpio_set_direction(wake_pin, RTC_GPIO_MODE_INPUT_ONLY) != ESP_OK ||
        rtc_gpio_pulldown_dis(wake_pin) != ESP_OK ||
        rtc_gpio_pullup_en(wake_pin) != ESP_OK ||
        esp_sleep_enable_ext0_wakeup(wake_pin, 0) != ESP_OK) {
        (void)bsp_display_backlight_on();
        if (power_shutdown_label_) lv_label_set_text(power_shutdown_label_, T("关机失败"));
        shutdown_armed_ = false;
        return;
    }
    esp_deep_sleep_start();
}

void VibeCoding::powerTimeoutCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    const size_t count = sizeof(kScreenOffOptions) / sizeof(kScreenOffOptions[0]);
    size_t index = 0;
    for (size_t i = 0; i < count; ++i)
        if (kScreenOffOptions[i] == self->screen_off_timeout_s_) index = i;
    self->screen_off_timeout_s_ = kScreenOffOptions[(index + 1) % count];
    saveScreenOffTimeoutImpl(self->screen_off_timeout_s_);
    self->renderPower();
}

void VibeCoding::renderVolume()
{
    if (!volume_value_label_) return;
    const int volume = bsp_extra_out_volume_get();
    if (volume <= 0) {
        lv_label_set_text(volume_value_label_, T("静音"));
        return;
    }
    char text[16];
    std::snprintf(text, sizeof text, "%d%%", volume);
    lv_label_set_text(volume_value_label_, text);
}

void VibeCoding::showVolume(bool visible)
{
    if (!volume_panel_) return;
    if (visible) {
        lv_obj_add_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(theme_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(power_panel_, LV_OBJ_FLAG_HIDDEN);
        if (firmware_panel_) lv_obj_add_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN);
        renderVolume();
        lv_obj_remove_flag(volume_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(volume_panel_);
    } else {
        lv_obj_add_flag(volume_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VibeCoding::volumeOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showVolume(true);
}

void VibeCoding::volumeBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showVolume(false);
}

void VibeCoding::adjustVolume(int delta)
{
    const int current = bsp_extra_out_volume_get();
    int next = current + delta;
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    if (next == current) return;
    if (bsp_extra_out_volume_set(next) != ESP_OK) return;
    renderVolume();
    if (next > 0) made_chime_preview();
}

void VibeCoding::volumeDownCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->adjustVolume(-10);
}

void VibeCoding::volumeUpCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->adjustVolume(10);
}

void VibeCoding::screenWakeCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->applyScreenOff(false);
}

void VibeCoding::applyScreenOff(bool off)
{
    if (off == screen_off_) return;
    screen_off_ = off;
    if (off) {
        // 全屏遮罩放在 LVGL 顶层：吸收点亮屏幕的那一次触摸，
        // 不会穿透到底下的按钮。
        power_off_overlay_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(power_off_overlay_);
        lv_obj_set_size(power_off_overlay_, made_screen_w(), made_screen_h());
        lv_obj_set_style_bg_color(power_off_overlay_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(power_off_overlay_, LV_OPA_COVER, 0);
        lv_obj_add_flag(power_off_overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(power_off_overlay_, screenWakeCallback, LV_EVENT_PRESSED, this);
        (void)bsp_display_backlight_off();
        ESP_LOGI("vibe_power", "Screen off after %u s idle", (unsigned)screen_off_timeout_s_);
    } else {
        if (power_off_overlay_) {
            lv_obj_delete(power_off_overlay_);
            power_off_overlay_ = nullptr;
        }
        (void)bsp_display_backlight_on();
        ESP_LOGI("vibe_power", "Screen on");
    }
}

void VibeCoding::themeRebootCallback(lv_timer_t *timer)
{
    (void)timer;
    esp_restart();
}

void VibeCoding::themeSelectCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    auto *button = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const char *selected = static_cast<const char *>(lv_obj_get_user_data(button));
    if (selected == nullptr || self->theme_list_loading_.load()) return;
    const auto pairing = vibe_pairing::snapshot();
    const bool usb_mode = pairing.access_mode == vibe_pairing::AccessMode::UsbDirect;
    std::string base = !pairing.url.empty() ? pairing.url : pairing.manual_url;
    if (!usb_mode && base.empty()) {
        if (self->theme_status_label_) lv_label_set_text(self->theme_status_label_, T("尚未连接电脑，无法应用"));
        return;
    }
    struct ApplyArgs {
        VibeCoding *self;
        std::string name;
        std::string base;
        bool usb_mode;
    };
    if (xTaskCreate([](void *arg) {
            auto *args = static_cast<ApplyArgs *>(arg);
            const esp_err_t result = args->usb_mode
                                         ? vibe_theme::applyFromBridge(args->name, "")
                                         : vibe_theme::applyFromBridge(args->name, args->base);
            if (result == ESP_OK) args->self->theme_reboot_pending_.store(true);
            delete args;
            vTaskDelete(nullptr);
        }, "vibe_theme_apply", 12288,
        new ApplyArgs{self, selected, base, usb_mode}, 3, nullptr) != pdPASS) {
        if (self->theme_status_label_) lv_label_set_text(self->theme_status_label_, T("应用任务启动失败"));
    }
}

void VibeCoding::themeSyncCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || self->theme_list_loading_.load()) return;
    const auto pairing = vibe_pairing::snapshot();
    const bool usb_mode = pairing.access_mode == vibe_pairing::AccessMode::UsbDirect;
    std::string base = !pairing.url.empty() ? pairing.url : pairing.manual_url;
    if (!usb_mode && base.empty()) {
        if (self->theme_status_label_) lv_label_set_text(self->theme_status_label_, T("尚未连接电脑，无法获取"));
        return;
    }
    self->theme_list_loading_.store(true);
    self->theme_rows_signature_ = "";  // 立即显示加载态
    struct ListArgs {
        VibeCoding *self;
        std::string base;
        bool usb_mode;
    };
    if (xTaskCreate([](void *arg) {
            auto *args = static_cast<ListArgs *>(arg);
            std::vector<vibe_theme::ThemeInfo> list;
            const bool ok = (args->usb_mode
                                 ? vibe_theme::fetchThemeList("", list)
                                 : vibe_theme::fetchThemeList(args->base, list)) == ESP_OK;
            ESP_LOGI("vibe_ui", "[task] list fetched ok=%d", ok);
            args->self->theme_list_loading_.store(false);  // 先清标志：后续任何卡顿都不拖累 UI 状态
            {
                std::lock_guard<std::mutex> lock(args->self->model_mutex_);
                args->self->theme_live_list_ =
                    ok ? std::move(list) : std::vector<vibe_theme::ThemeInfo>{};
            }
            ESP_LOGI("vibe_ui", "[task] list published");
            delete args;
            vTaskDelete(nullptr);
        }, "vibe_theme_list", 12288, new ListArgs{self, base, usb_mode}, 3, nullptr) != pdPASS) {
        self->theme_list_loading_.store(false);
        if (self->theme_status_label_) lv_label_set_text(self->theme_status_label_, T("列表任务启动失败"));
    }
}

void VibeCoding::themeBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showTheme(false);
}

void VibeCoding::themeOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showTheme(true);
}

void VibeCoding::showTheme(bool visible)
{
    if (!theme_panel_) return;
    if (visible) {
        lv_obj_add_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
        if (firmware_panel_) lv_obj_add_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(theme_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(theme_panel_);
        // 打开即实时拉取主题列表。
        if (!theme_list_loading_.load()) {
            const auto pairing = vibe_pairing::snapshot();
            const bool usb_mode = pairing.access_mode == vibe_pairing::AccessMode::UsbDirect;
            std::string base = !pairing.url.empty() ? pairing.url : pairing.manual_url;
            if (usb_mode || !base.empty()) {
                theme_list_loading_.store(true);
                theme_rows_signature_ = "";
                struct ListArgs {
                    VibeCoding *self;
                    std::string base;
                    bool usb_mode;
                };
                if (xTaskCreate([](void *arg) {
                        auto *args = static_cast<ListArgs *>(arg);
                        std::vector<vibe_theme::ThemeInfo> list;
                        const bool ok = (args->usb_mode
                                             ? vibe_theme::fetchThemeList("", list)
                                             : vibe_theme::fetchThemeList(args->base, list)) == ESP_OK;
                        ESP_LOGI("vibe_ui", "[task] list fetched ok=%d", ok);
                        args->self->theme_list_loading_.store(false);  // 先清标志：后续任何卡顿都不拖累 UI 状态
                        {
                            std::lock_guard<std::mutex> lock(args->self->model_mutex_);
                            args->self->theme_live_list_ =
                                ok ? std::move(list) : std::vector<vibe_theme::ThemeInfo>{};
                        }
                        ESP_LOGI("vibe_ui", "[task] list published");
                        delete args;
                        vTaskDelete(nullptr);
                    }, "vibe_theme_list", 12288, new ListArgs{this, base, usb_mode}, 3, nullptr) != pdPASS) {
                    theme_list_loading_.store(false);
                }
            }
        }
        renderThemes();
    } else {
        lv_obj_add_flag(theme_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VibeCoding::renderThemes()
{
    if (!theme_panel_ || lv_obj_has_flag(theme_panel_, LV_OBJ_FLAG_HIDDEN)) return;
    const auto sync = vibe_theme::syncStatus();
    bool loading = theme_list_loading_.load();
    // 看门狗：加载超过 45 秒强制结束，避免状态行永久卡住。
    if (loading) {
        if (theme_loading_since_ms_ == 0) theme_loading_since_ms_ = static_cast<int32_t>(lv_tick_get());
        else if (lv_tick_get() - theme_loading_since_ms_ > 45000) {
            ESP_LOGW("vibe_ui", "[render] loading watchdog fired");
            theme_list_loading_.store(false);
            loading = false;
            theme_loading_since_ms_ = 0;
            std::lock_guard<std::mutex> lock(model_mutex_);
            theme_live_list_.clear();
        }
    } else {
        theme_loading_since_ms_ = 0;
    }
    std::vector<vibe_theme::ThemeInfo> themes;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        themes = theme_live_list_;
    }
    static bool logged_loading = false;
    if (loading != logged_loading) {
        logged_loading = loading;
        ESP_LOGI("vibe_ui", "[render] loading=%d msg='%s' list=%zu", loading,
                 sync.message.c_str(), themes.size());
    }
    if (theme_status_label_) {
        std::string status = sync.message;
        if (loading && status.empty())
            status = T(themes.empty() ? "正在获取主题列表…" : "正在刷新列表…");
        lv_label_set_text(theme_status_label_, status.c_str());
    }
    // 应用成功后安排重启（LVGL 任务内创建一次性定时器）。
    if (theme_reboot_pending_.load() && !theme_reboot_started_.exchange(true)) {
        lv_timer_t *timer = lv_timer_create(themeRebootCallback, 1200, nullptr);
        lv_timer_set_repeat_count(timer, 1);
    }
    std::string signature = vibe_theme::activeName() + "|" + std::to_string(themes.size()) +
                            "|" + std::to_string(loading ? 1 : 0) + "|" + sync.message;
    for (const auto &theme : themes) signature += "|" + theme.name;
    if (signature == theme_rows_signature_) return;
    theme_rows_signature_ = signature;

    lv_obj_clean(theme_rows_);
    // 第一行：内置默认。
    {
        const bool active = vibe_theme::activeName().empty();
        lv_obj_t *button = touchButton(theme_rows_, 16, 4, 248, 40,
            nullptr, themeSelectCallback, this, active ? VT(accent) : VT(bg_row));
        lv_obj_set_overflow_visible(button, false);
        lv_obj_set_user_data(button, strdup(""));
        lv_obj_add_event_cb(button, [](lv_event_t *event) {
            if (lv_event_get_code(event) == LV_EVENT_DELETE) {
                auto *name = static_cast<char *>(lv_obj_get_user_data(lv_event_get_target(event)));
                if (name != nullptr) free(name);
            }
        }, LV_EVENT_DELETE, nullptr);
        lv_obj_t *name = settingsLabel(button, T("默认主题（当前界面）"), 10, 11, 228,
                                       active ? VT(text_primary) : VT(text_secondary), false);
        lv_obj_set_style_text_font(name, text_font, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        // 按钮刚创建还没布局，lv_obj_get_width 会读到 0，字会整行消失；
        // 宽度直接用设计值：按钮 248，两侧各留 8。
        lv_obj_set_width(name, made_s(232));
        // 固定一行高：DOT 截断只在文字高度超过标签高度时生效，
        // 高度自适应会让长主题名换行溢出按钮。
        lv_obj_set_height(name, lv_font_get_line_height(text_font));
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
    }
    int row = 1;
    for (const auto &theme : themes) {
        const int y = 4 + row * 46;
        const bool active = vibe_theme::activeName() == theme.name;
        lv_obj_t *button = touchButton(theme_rows_, 16, y, 248, 40,
            nullptr, themeSelectCallback, this, active ? VT(accent) : VT(bg_row));
        lv_obj_set_overflow_visible(button, false);
        lv_obj_set_user_data(button, strdup(theme.name.c_str()));
        lv_obj_add_event_cb(button, [](lv_event_t *event) {
            if (lv_event_get_code(event) == LV_EVENT_DELETE) {
                auto *name = static_cast<char *>(lv_obj_get_user_data(lv_event_get_target(event)));
                if (name != nullptr) free(name);
            }
        }, LV_EVENT_DELETE, nullptr);
        char title[96];
        std::snprintf(title, sizeof(title), "%s%s", theme.title.c_str(), active ? T(" · 使用中") : "");
        lv_obj_t *name = settingsLabel(button, title, 10, 11, 228,
                                       active ? VT(text_primary) : VT(text_secondary), false);
        lv_obj_set_style_text_font(name, text_font, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, made_s(232));
        lv_obj_set_height(name, lv_font_get_line_height(text_font));
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        ++row;
    }
}

void VibeCoding::accessOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showAccess(true);
}

void VibeCoding::accessBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) {
        if (self->access_editor_panel_ && !lv_obj_has_flag(self->access_editor_panel_, LV_OBJ_FLAG_HIDDEN))
            self->closeAccessEditor(false);
        else if (self->receiver_picker_ && !lv_obj_has_flag(self->receiver_picker_, LV_OBJ_FLAG_HIDDEN))
            self->showReceiverScan(false);
        else self->showAccess(false);
    }
}

void VibeCoding::accessPickerCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    self->access_picker_open_ = true;
    self->updateAccessModeButtons();
}

void VibeCoding::phoneSetupOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self && self->canOperate()) self->showPhoneSetup(true);
}

void VibeCoding::phoneSetupBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showPhoneSetup(false);
}

void VibeCoding::showPhoneSetup(bool visible)
{
    if (!phone_setup_panel_) return;
    phone_setup_requested_.store(false);
    phone_setup_epoch_.fetch_add(1);
    phone_setup_result_.store(0);
    if (!visible) {
        lv_obj_add_flag(phone_setup_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(phone_setup_password_, "");
        return;
    }
    showReceiverScan(false);
    closeAccessEditor(false);
    {
        std::lock_guard<std::mutex> lock(voice_action_mutex_);
        vibe_voice::cancel();
    }
    phone_setup_revision_ = UINT32_MAX;
    lv_obj_add_flag(phone_setup_qr_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(phone_setup_hint_, T("正在开启手机配置…"));
    made_page_label(phone_setup_hint_, 55, 142, 250);
    lv_label_set_text(phone_setup_network_, "");
    lv_label_set_text(phone_setup_password_, "");
    lv_label_set_text(phone_setup_address_, "");
    lv_obj_remove_flag(phone_setup_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(phone_setup_panel_);
    phone_setup_requested_.store(true);
}

void VibeCoding::renderPhoneSetup()
{
    if (!phone_setup_panel_ || lv_obj_has_flag(phone_setup_panel_, LV_OBJ_FLAG_HIDDEN)) return;
    const int result = phone_setup_result_.exchange(0);
    if (result == 1) {
        showSettings(false);
        drawn_revision_ = UINT32_MAX;
        return;
    }
    if (result == -1) {
        std::string error;
        { std::lock_guard<std::mutex> lock(model_mutex_); error = phone_setup_error_; }
        lv_obj_add_flag(phone_setup_qr_, LV_OBJ_FLAG_HIDDEN);
        made_page_label(phone_setup_hint_, 55, 142, 250);
        lv_label_set_text(phone_setup_hint_, error.c_str());
        lv_label_set_text(phone_setup_network_, "");
        lv_label_set_text(phone_setup_password_, "");
        lv_label_set_text(phone_setup_address_, T("返回后可重新开启"));
        return;
    }
    if (!phone_setup_requested_.load() || phone_setup_ready_epoch_.load() != phone_setup_epoch_.load()) return;
    const auto phone = vibe_phone_setup::snapshot();
    if (phone.revision == phone_setup_revision_) return;
    phone_setup_revision_ = phone.revision;
    if (phone.phase == vibe_phone_setup::Phase::Ready) {
        made_page_label(phone_setup_hint_, 55, 78, 250);
        lv_label_set_text(phone_setup_hint_, T("手机扫码连接热点"));
        const lv_result_t result = lv_qrcode_update(phone_setup_qr_, phone.qr_payload.data(), phone.qr_payload.size());
        if (result == LV_RESULT_OK) lv_obj_remove_flag(phone_setup_qr_, LV_OBJ_FLAG_HIDDEN);
        else lv_label_set_text(phone_setup_hint_, T("在手机 Wi-Fi 里连接下方热点"));
        lv_label_set_text(phone_setup_network_, phone.ssid.c_str());
        lv_label_set_text(phone_setup_password_, (std::string(T("密码")) + ": " + phone.password).c_str());
        lv_label_set_text(phone_setup_address_, "192.168.8.1");
    } else if (phone.phase == vibe_phone_setup::Phase::Submitted) {
        lv_obj_add_flag(phone_setup_qr_, LV_OBJ_FLAG_HIDDEN);
        made_page_label(phone_setup_hint_, 55, 142, 250);
        lv_label_set_text(phone_setup_hint_, T("已收到，正在应用设置…"));
        lv_label_set_text(phone_setup_network_, "");
        lv_label_set_text(phone_setup_password_, "");
        lv_label_set_text(phone_setup_address_, "");
    }
}

bool VibeCoding::applyPhoneSetup(const vibe_phone_setup::Submission &input, std::string &error)
{
    // The HTTP worker has finished its response and the setup AP is already
    // stopped. Reuse the same persisted modes and six-digit pairing as touch.
    bool saved = false;
    if (input.mode == "receiver") {
        if (vibe_wifi::save_receiver_credentials(input.receiver_ssid.c_str(), input.receiver_password.c_str()) != ESP_OK)
            error = T("保存接收端热点信息失败");
        else if (!vibe_pairing::use_receiver()) error = T("保存接收端模式失败");
        else if (vibe_wifi::connect_receiver() != ESP_OK) error = T("连接接收端热点失败，请核对信息");
        else saved = true;
    } else {
        if (vibe_wifi::receiver_active() && vibe_wifi::restore_station() != ESP_OK) {
            error = T("恢复原 Wi-Fi 失败，请重试");
            return false;
        }
        if (!input.wifi_ssid.empty() && vibe_wifi::save_credentials(input.wifi_ssid.c_str(), input.wifi_password.c_str()) != ESP_OK) {
            error = T("保存 Wi-Fi 失败，请重试");
            return false;
        }
        if (input.mode == "manual")
            saved = vibe_pairing::set_manual_endpoint(input.host, input.port, input.https, error);
        else if (input.mode == "automatic") {
            saved = vibe_pairing::use_automatic_discovery();
            if (!saved) error = T("保存自动发现设置失败");
        }
    }
    if (!saved) return false;
    vibe_usb::set_active(false);
    invalidateCatalog(true);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.sessions.clear();
        model_.tasks.clear();
        model_.connection_error = input.mode == "receiver" ? T("正在连接 USB 接收端") : T("正在连接电脑");
        for (auto &provider : providers_) provider.selected_session_id.clear();
        ++model_.revision;
    }
    return true;
}

void VibeCoding::showSettings(bool visible)
{
    if (!settings_overlay_) return;
    showPhoneSetup(false);
    showWifiScan(false);
    settings_open_ = visible;
    showReceiverScan(false);
    button_epoch_.fetch_add(1);
    if (visible) {
        lv_obj_remove_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(access_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(language_panel_, LV_OBJ_FLAG_HIDDEN);
        if (volume_panel_) lv_obj_add_flag(volume_panel_, LV_OBJ_FLAG_HIDDEN);
        if (power_panel_) lv_obj_add_flag(power_panel_, LV_OBJ_FLAG_HIDDEN);
        if (firmware_panel_) lv_obj_add_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(settings_overlay_);
    } else {
        lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VibeCoding::showLanguage(bool visible)
{
    if (!language_panel_) return;
    if (visible) {
        lv_obj_add_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(access_panel_, LV_OBJ_FLAG_HIDDEN);
        if (firmware_panel_) lv_obj_add_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(language_panel_, LV_OBJ_FLAG_HIDDEN);
        refreshLanguage();
    } else {
        lv_obj_add_flag(language_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VibeCoding::showAccess(bool visible)
{
    if (!access_panel_) return;
    if (!visible) {
        showReceiverScan(false);
        showWifiScan(false);
        closeAccessEditor(false);
        lv_obj_add_flag(access_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const auto pairing = vibe_pairing::snapshot();
    access_manual_selected_ = pairing.access_mode == vibe_pairing::AccessMode::Manual;
    access_receiver_selected_ = pairing.access_mode == vibe_pairing::AccessMode::Receiver;
    access_usb_selected_ = pairing.access_mode == vibe_pairing::AccessMode::UsbDirect;
    access_https_selected_ = pairing.manual_url.empty() ||
        pairing.manual_url.rfind("https://", 0) == 0;
    std::string host;
    std::string port = access_https_selected_ ? "443" : "8788";
    if (!pairing.manual_url.empty()) {
        const size_t scheme = pairing.manual_url.find("://");
        const std::string authority = scheme == std::string::npos ? pairing.manual_url :
            pairing.manual_url.substr(scheme + 3);
        const size_t colon = authority.rfind(':');
        host = colon == std::string::npos ? authority : authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    if (access_receiver_selected_) {
        char ssid[33] = {};
        if (vibe_wifi::receiver_ssid(ssid, sizeof(ssid))) host = ssid;
        else host.clear();
    }
    lv_textarea_set_max_length(access_host_input_, access_receiver_selected_ ? 32 : 100);
    lv_textarea_set_text(access_host_input_, host.c_str());
    lv_textarea_set_text(access_port_input_, port.c_str());
    lv_textarea_set_text(access_password_input_, "");
    lv_obj_add_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(language_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(access_panel_, LV_OBJ_FLAG_HIDDEN);
    access_picker_open_ = true;
    updateAccessModeButtons();
}

void VibeCoding::updateAccessModeButtons()
{
    if (!access_panel_) return;
    // 选中态：accent 实底 + text_on_accent；未选中回到 ghost（透明底）。
    auto setAccessSelected = [](lv_obj_t *button, bool selected) {
        lv_obj_set_style_bg_color(button, lv_color_hex(selected ? VT(accent) : VT(btn_normal)), 0);
        lv_obj_set_style_bg_opa(button, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (button && lv_obj_get_child_cnt(button) > 0) {
            lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                        lv_color_hex(selected ? VT(text_on_accent) : VT(text_primary)), 0);
        }
    };
    setAccessSelected(access_auto_button_,
        !(access_manual_selected_ || access_receiver_selected_ || access_usb_selected_));
    setAccessSelected(access_manual_button_, access_manual_selected_);
    setAccessSelected(access_receiver_button_, access_receiver_selected_);
    setAccessSelected(access_usb_button_, access_usb_selected_);
    for (lv_obj_t *item : {access_auto_button_, access_manual_button_,
                          access_receiver_button_, access_usb_button_, access_picker_hint_, phone_setup_button_}) {
        if (access_picker_open_) lv_obj_remove_flag(item, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t *item : {access_save_button_, access_change_button_, access_usb_hint_,
                          access_host_input_, access_port_input_, access_password_input_,
                          access_https_button_, receiver_scan_button_}) {
        if (access_picker_open_) lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(item, LV_OBJ_FLAG_HIDDEN);
    }
    made_page_label(access_status_label_, access_picker_open_ ? 70 : 47,
                    access_picker_open_ ? 269 : 109, access_picker_open_ ? 220 : 266);
    if (access_picker_open_) lv_obj_add_flag(access_status_label_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(access_status_label_, LV_OBJ_FLAG_HIDDEN);
    if (access_picker_open_) {
        lv_label_set_text(access_status_label_, T("选中后设置，保存后生效"));
        return;
    }
    const char *mode_name = access_usb_selected_ ? T("USB 直连") : access_receiver_selected_ ?
                            T("接收端") : access_manual_selected_ ? T("手动地址") : "Wi-Fi";
    lv_label_set_text(access_change_label_, (std::string(mode_name) + T(" · 更换方式")).c_str());
    if (access_usb_selected_) lv_obj_remove_flag(access_usb_hint_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(access_usb_hint_, LV_OBJ_FLAG_HIDDEN);
    if (access_manual_selected_ || access_receiver_selected_)
        lv_obj_remove_flag(access_host_input_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(access_host_input_, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t *item : {access_port_input_, access_https_button_}) {
        if (access_manual_selected_) lv_obj_remove_flag(item, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
    }
    if (access_receiver_selected_) lv_obj_remove_flag(receiver_scan_button_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(receiver_scan_button_, LV_OBJ_FLAG_HIDDEN);
    if (access_receiver_selected_) lv_obj_remove_flag(access_password_input_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(access_password_input_, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_max_length(access_host_input_, access_receiver_selected_ ? 32 : 100);
    lv_textarea_set_placeholder_text(access_host_input_,
        access_receiver_selected_ ? T("接收端热点名称 SSID") : T("主机名或 IPv4 地址"));
    lv_label_set_text(access_https_label_, access_https_selected_ ? T("HTTPS · 开") : T("HTTPS · 关"));
    if (access_usb_selected_)
        lv_label_set_text(access_status_label_, T("USB 直连，无需 Wi-Fi"));
    else if (access_receiver_selected_)
        lv_label_set_text(access_status_label_, T("点选接收端，再输入热点密码"));
    else if (!access_manual_selected_) {
        if (!vibe_wifi::receiver_active() && vibe_wifi::is_connected()) {
            wifi_ap_record_t ap = {};
            std::string status = T("已连接 Wi-Fi");
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                char ssid[33] = {};
                std::memcpy(ssid, ap.ssid, sizeof(ap.ssid));
                status += std::string("：") + ssid;
            }
            lv_label_set_text(access_status_label_, status.c_str());
        } else {
            lv_label_set_text(access_status_label_, T("请先在网络设置连接 Wi-Fi"));
        }
    }
    else
        lv_label_set_text(access_status_label_, T("公网地址请选择 HTTPS"));
}

void VibeCoding::accessModeCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    const bool was_receiver = self->access_receiver_selected_;
    const bool was_manual = self->access_manual_selected_;
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    self->access_manual_selected_ = target == self->access_manual_button_;
    self->access_receiver_selected_ = target == self->access_receiver_button_;
    self->access_usb_selected_ = target == self->access_usb_button_;
    lv_textarea_set_max_length(self->access_host_input_, self->access_receiver_selected_ ? 32 : 100);
    if (self->access_receiver_selected_ && !was_receiver) {
        char ssid[33] = {};
        (void)vibe_wifi::receiver_ssid(ssid, sizeof(ssid));
        lv_textarea_set_text(self->access_host_input_, ssid);
        lv_textarea_set_text(self->access_password_input_, "");
    } else if (self->access_manual_selected_ && !was_manual) {
        const std::string url = vibe_pairing::snapshot().manual_url;
        const size_t scheme = url.find("://");
        const std::string authority = scheme == std::string::npos ? url : url.substr(scheme + 3);
        const size_t colon = authority.rfind(':');
        lv_textarea_set_text(self->access_host_input_,
            (colon == std::string::npos ? authority : authority.substr(0, colon)).c_str());
        lv_textarea_set_text(self->access_port_input_,
            colon == std::string::npos ? "443" : authority.substr(colon + 1).c_str());
    }
    self->access_picker_open_ = false;
    self->updateAccessModeButtons();
    if (self->access_receiver_selected_) self->showReceiverScan(true);
}

void VibeCoding::accessFieldCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->access_editor_panel_) return;
    lv_obj_t *field = static_cast<lv_obj_t *>(lv_event_get_target(event));
    self->access_editor_target_ = field;
    const bool port = field == self->access_port_input_;
    const bool password = field == self->access_password_input_;
    lv_label_set_text(self->access_editor_title_, port ? T("端口") : password ? T("密码") :
                      self->access_receiver_selected_ ? "SSID" : T("地址"));
    lv_textarea_set_password_mode(self->access_editor_input_, password);
    lv_textarea_set_max_length(self->access_editor_input_, lv_textarea_get_max_length(field));
    lv_textarea_set_accepted_chars(self->access_editor_input_, port ? "0123456789" : nullptr);
    lv_textarea_set_text(self->access_editor_input_, lv_textarea_get_text(field));
    if (self->access_keyboard_) lv_obj_delete(self->access_keyboard_);
    const int keyboard_y = made_page_y(157);
    self->access_keyboard_ = vibe_touch_keyboard::create(self->access_editor_panel_,
        self->access_editor_input_, made_page_x(36), keyboard_y, made_page_w(288),
        made_screen_h() - keyboard_y - 8, port, password && self->access_receiver_selected_);
    lv_obj_remove_flag(self->access_editor_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(self->access_editor_panel_);
}

void VibeCoding::closeAccessEditor(bool save)
{
    if (!access_editor_panel_) return;
    const bool was_wifi_password = access_editor_target_ == wifi_password_input_;
    if (save && access_editor_target_ && access_editor_input_)
        lv_textarea_set_text(access_editor_target_, lv_textarea_get_text(access_editor_input_));
    access_editor_target_ = nullptr;
    lv_obj_add_flag(access_editor_panel_, LV_OBJ_FLAG_HIDDEN);
    if (save && was_wifi_password) connectSelectedWifi();
}

void VibeCoding::accessEditorDoneCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    // The keyboard can dispatch READY from inside its own event callback.
    // Defer the UI transition until that dispatch has returned.
    lv_async_call([](void *arg) { static_cast<VibeCoding *>(arg)->closeAccessEditor(true); }, self);
}

void VibeCoding::accessEditorCancelCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->closeAccessEditor(false);
}

void VibeCoding::receiverScanOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showReceiverScan(true);
}

void VibeCoding::receiverScanBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showReceiverScan(false);
}

void VibeCoding::receiverScanRefreshCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    (void)vibe_wifi::request_receiver_scan();
    self->receiver_scan_revision_ = UINT32_MAX;
    self->renderReceiverScan();
}

void VibeCoding::wifiScanOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showWifiScan(true);
}

void VibeCoding::wifiScanBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showWifiScan(false);
}

void VibeCoding::wifiScanRefreshCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    self->startWifiScan();
    self->renderWifiNetworks();
}

void VibeCoding::wifiNetworkCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    // 按钮在列表中的位置即 1 起始的网络序号（列表为空时只有提示行，无按钮）。
    const uintptr_t index = (uintptr_t)lv_obj_get_index(lv_event_get_target(event)) + 1;
    ESP_LOGI("vibe_ui", "wifi row clicked: index=%u known=%u", (unsigned)index,
             (unsigned)self->wifi_scan_snapshot_.networks.size());
    if (index == 0) return;
    std::string ssid;
    bool open = false;
    {
        const auto &networks = self->wifi_scan_snapshot_.networks;
        if (index > networks.size()) return;
        ssid = networks[index - 1].ssid;
        open = networks[index - 1].open;
    }
    ESP_LOGI("vibe_ui", "wifi selected: %s (open=%d)", ssid.c_str(), (int)open);
    self->wifi_selected_ssid_ = ssid;
    if (open) {
        // 开放网络无需密码，直接保存并连接。
        if (vibe_wifi::save_credentials(ssid.c_str(), "") == ESP_OK && self->wifi_scan_status_) {
            self->wifi_join_ssid_ = ssid;
            self->wifi_join_pending_ = true;
            lv_label_set_text(self->wifi_scan_status_, (std::string(T("正在连接 ")) + ssid + T(" …")).c_str());
        }
        return;
    }
    // 加密网络：弹出编辑器输入密码，完成后由 closeAccessEditor 触发连接。
    self->access_editor_target_ = self->wifi_password_input_;
    lv_label_set_text(self->access_editor_title_,
                      (std::string(T("Wi-Fi 密码：")) + ssid).c_str());
    lv_textarea_set_password_mode(self->access_editor_input_, true);
    lv_textarea_set_max_length(self->access_editor_input_, 63);
    lv_textarea_set_accepted_chars(self->access_editor_input_, nullptr);
    lv_textarea_set_text(self->access_editor_input_, "");
    if (self->access_keyboard_) lv_obj_delete(self->access_keyboard_);
    const int keyboard_y = made_page_y(157);
    self->access_keyboard_ = vibe_touch_keyboard::create(self->access_editor_panel_,
        self->access_editor_input_, made_page_x(36), keyboard_y, made_page_w(288),
        made_screen_h() - keyboard_y - 8, false, true);
    lv_obj_remove_flag(self->access_editor_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(self->access_editor_panel_);
}

void VibeCoding::startWifiScan()
{
    // 扫描所有权在 vibe_wifi 内部：station_mutex 串行化、暂停重连、
    // 断开未完成的关联后非阻塞扫描，SCAN_DONE 事件回填快照。
    (void)vibe_wifi::request_station_scan();
}

void VibeCoding::renderWifiNetworks()
{
    if (!wifi_picker_ || lv_obj_has_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN)) return;
    if (wifi_scan_status_ && wifi_join_pending_) {
        if (!vibe_wifi::receiver_active() && vibe_wifi::is_connected()) {
            wifi_ap_record_t ap = {};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                char ssid[33] = {};
                std::memcpy(ssid, ap.ssid, sizeof(ap.ssid));
                if (wifi_join_ssid_.empty() || wifi_join_ssid_ == ssid) {
                    lv_label_set_text(wifi_scan_status_, (std::string(T("已连接 Wi-Fi")) + "：" + ssid).c_str());
                    wifi_join_pending_ = false;
                }
            }
        } else {
            const int reason = vibe_wifi::disconnect_reason();
            const bool auth_failed = reason == WIFI_REASON_AUTH_EXPIRE ||
                reason == WIFI_REASON_MIC_FAILURE ||
                reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                reason == WIFI_REASON_AUTH_FAIL ||
                reason == WIFI_REASON_ASSOC_FAIL ||
                reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                reason == WIFI_REASON_CONNECTION_FAIL;
            const bool missing = reason == WIFI_REASON_NO_AP_FOUND ||
                reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
                reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
                reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
            if (auth_failed) {
                lv_label_set_text(wifi_scan_status_, T("连接失败，请核对密码"));
                wifi_join_pending_ = false;
            } else if (missing) {
                lv_label_set_text(wifi_scan_status_, T("找不到这个网络"));
                wifi_join_pending_ = false;
            }
        }
    }
    wifi_scan_snapshot_ = vibe_wifi::station_scan_snapshot();
    const uint32_t revision = wifi_scan_snapshot_.generation;
    const bool scanning = wifi_scan_snapshot_.state == vibe_wifi::ReceiverScanState::Scanning;
    // 扫描期间每帧都要重绘（提示行显示"正在扫描…"），完成后按 generation 去重。
    if (!scanning && revision == wifi_scan_list_revision_) return;
    wifi_scan_list_revision_ = revision;
    const auto &networks = wifi_scan_snapshot_.networks;
    lv_obj_clean(wifi_network_list_);
    lv_obj_scroll_to_y(wifi_network_list_, 0, LV_ANIM_OFF);
    if (networks.empty()) {
        char hint[96];
        const int scan_error = (int)wifi_scan_snapshot_.error;
        if (scanning) {
            std::snprintf(hint, sizeof(hint), "%s", T("正在扫描…"));
        } else if (scan_error != 0) {
            std::snprintf(hint, sizeof(hint), "%s 0x%x)", T("扫描失败，点刷新重试 ("), (unsigned)scan_error);
        } else {
            std::snprintf(hint, sizeof(hint), "%s", T("未发现网络，点刷新重试"));
        }
        lv_obj_t *hint_label = settingsLabel(wifi_network_list_, hint, 8, 10, 254, VT(text_secondary), false);
        lv_obj_set_style_text_font(hint_label, text_font, 0);
        return;
    }
    // 与接收端选择器完全相同的成熟模式：touchButton + 手动 y 定位 + 两行文字。
    // lv_list_add_button 的行在此触摸屏上收不到事件（按下有高亮、无回调）。
    int row = 0;
    for (const auto &network : networks) {
        const int y = 4 + row * 56;
        const int strength = network.rssi <= -100 ? 0 : network.rssi >= -50 ? 100 : 2 * (network.rssi + 100);
        char sub[48];
        std::snprintf(sub, sizeof(sub), "%d%%  %s", strength, network.open ? T("开放") : T("已加密"));
        lv_obj_t *button = touchButton(wifi_network_list_, 8, y, 254, 48,
                                       nullptr, wifiNetworkCallback, this, VT(bg_row));
        lv_obj_t *name = settingsLabel(button, network.ssid.c_str(), 10, 4, 234, VT(text_primary), false);
        lv_obj_set_style_text_font(name, text_font, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *sub_label = settingsLabel(button, sub, 10, 26, 234, VT(text_secondary), false);
        lv_obj_set_style_text_font(sub_label, text_font, 0);
        lv_obj_remove_flag(sub_label, LV_OBJ_FLAG_CLICKABLE);
        ++row;
    }
}

void VibeCoding::showWifiScan(bool visible)
{
    if (!wifi_picker_) return;
    if (!visible) {
        const bool was_open = !lv_obj_has_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN);
        if (was_open && settings_home_) lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (settings_home_) lv_obj_add_flag(settings_home_, LV_OBJ_FLAG_HIDDEN);
    if (access_panel_) lv_obj_add_flag(access_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_picker_);
    if (wifi_scan_status_) {
        if (!vibe_wifi::receiver_active() && vibe_wifi::is_connected()) {
            wifi_ap_record_t ap = {};
            std::string status = T("已连接 Wi-Fi");
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                char ssid[33] = {};
                std::memcpy(ssid, ap.ssid, sizeof(ap.ssid));
                status += std::string("：") + ssid;
            }
            lv_label_set_text(wifi_scan_status_, status.c_str());
        } else {
            lv_label_set_text(wifi_scan_status_, T("选择要加入的网络"));
        }
    }
    wifi_scan_list_revision_ = UINT32_MAX;
    startWifiScan();
    renderWifiNetworks();
}

void VibeCoding::connectSelectedWifi()
{
    if (wifi_selected_ssid_.empty()) return;
    const std::string password = wifi_password_input_ ? lv_textarea_get_text(wifi_password_input_) : "";
    if (vibe_wifi::save_credentials(wifi_selected_ssid_.c_str(), password.c_str()) != ESP_OK) {
        if (wifi_scan_status_) lv_label_set_text(wifi_scan_status_, T("保存 Wi-Fi 失败，请重试"));
        return;
    }
    if (wifi_scan_status_)
        lv_label_set_text(wifi_scan_status_, (std::string(T("正在连接 ")) + wifi_selected_ssid_ + T(" …")).c_str());
    wifi_join_ssid_ = wifi_selected_ssid_;
    wifi_join_pending_ = true;
    wifi_selected_ssid_.clear();
}

void VibeCoding::showReceiverScan(bool visible)
{
    if (!visible) vibe_wifi::cancel_receiver_scan();
    if (!receiver_picker_) return;
    receiver_pressed_button_ = nullptr;
    if (!visible) {
        lv_obj_add_flag(receiver_picker_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(receiver_picker_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(receiver_picker_);
    receiver_scan_revision_ = UINT32_MAX;
    (void)vibe_wifi::request_receiver_scan();
    renderReceiverScan();
}

void VibeCoding::renderReceiverScan()
{
    if (!receiver_picker_ || lv_obj_has_flag(receiver_picker_, LV_OBJ_FLAG_HIDDEN)) return;
    const auto scan = vibe_wifi::receiver_scan_snapshot();
    if (receiver_scan_revision_ == scan.generation) return;
    receiver_scan_revision_ = scan.generation;
    const bool scanning = scan.state == vibe_wifi::ReceiverScanState::Scanning;
    if (scanning) lv_obj_add_state(receiver_refresh_button_, LV_STATE_DISABLED);
    else lv_obj_remove_state(receiver_refresh_button_, LV_STATE_DISABLED);
    receiver_pressed_button_ = nullptr;
    lv_obj_clean(receiver_list_);
    receiver_rows_.clear();
    lv_obj_scroll_to_y(receiver_list_, 0, LV_ANIM_OFF);
    for (const auto &network : scan.networks) {
        const int y = 4 + static_cast<int>(receiver_rows_.size()) * 64;
        lv_obj_t *button = touchButton(receiver_list_, 8, y, 254, 54,
            nullptr, receiverSelectCallback, this);
        lv_obj_add_event_cb(button, receiverSelectCallback, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(button, receiverSelectCallback, LV_EVENT_PRESSING, this);
        lv_obj_t *name = settingsLabel(button, network.ssid.c_str(), 10, 7, 234, VT(text_primary), false);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_set_height(name, made_s(20));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        const std::string strength = std::to_string(network.rssi) + " dBm";
        lv_obj_t *signal = settingsLabel(button, strength.c_str(), 10, 31, 234, VT(text_secondary), false);
        lv_obj_set_style_text_font(signal, &lv_font_montserrat_14, 0);
        lv_obj_remove_flag(signal, LV_OBJ_FLAG_CLICKABLE);
        if (scanning) lv_obj_add_state(button, LV_STATE_DISABLED);
        receiver_rows_.push_back({button, network.ssid});
    }
    lv_label_set_text(receiver_hint_, scanning ? T("正在扫描接收端…") :
        scan.state == vibe_wifi::ReceiverScanState::Failed ? T("扫描失败，请点刷新") :
        scan.networks.empty() ? T("未找到，请接通接收端电源") : T("点选后输入电脑显示的密码"));
}

void VibeCoding::receiverSelectCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *input = lv_indev_get_act();
    if (code == LV_EVENT_PRESSED) {
        self->receiver_pressed_button_ = target;
        self->receiver_press_moved_ = false;
        if (input) lv_indev_get_point(input, &self->receiver_press_point_);
        return;
    }
    if (self->receiver_pressed_button_ != target) return;
    if (input) {
        lv_point_t point{};
        lv_indev_get_point(input, &point);
        if (std::abs(point.x - self->receiver_press_point_.x) > 12 ||
            std::abs(point.y - self->receiver_press_point_.y) > 12)
            self->receiver_press_moved_ = true;
    }
    if (code != LV_EVENT_CLICKED) return;
    self->receiver_pressed_button_ = nullptr;
    if (self->receiver_press_moved_ ||
        (input && lv_indev_get_gesture_dir(input) != LV_DIR_NONE)) return;
    for (const auto &row : self->receiver_rows_) {
        if (row.button != target) continue;
        const std::string ssid = row.ssid;
        if (ssid != lv_textarea_get_text(self->access_host_input_))
            lv_textarea_set_text(self->access_password_input_, "");
        lv_textarea_set_text(self->access_host_input_, ssid.c_str());
        self->showReceiverScan(false);
        // Keep remembered credentials: a previously paired receiver needs no
        // password re-entry. The field stays editable if its password changed.
        char saved_ssid[33] = {};
        const bool known = vibe_wifi::receiver_ssid(saved_ssid, sizeof(saved_ssid)) && ssid == saved_ssid;
        if (!known) lv_obj_send_event(self->access_password_input_, LV_EVENT_CLICKED, nullptr);
        return;
    }
}

void VibeCoding::accessHttpsCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    const std::string old_port = lv_textarea_get_text(self->access_port_input_);
    self->access_https_selected_ = !self->access_https_selected_;
    if (self->access_https_selected_ && old_port == "8788")
        lv_textarea_set_text(self->access_port_input_, "443");
    else if (!self->access_https_selected_ && old_port == "443")
        lv_textarea_set_text(self->access_port_input_, "8788");
    self->updateAccessModeButtons();
}

void VibeCoding::accessSaveCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || self->access_picker_open_) return;
    bool saved = false;
    std::string error;
    if (self->access_usb_selected_) {
        if (vibe_usb::initialize() != ESP_OK) error = T("USB 初始化失败，请重启后重试");
        else {
            saved = vibe_pairing::use_usb_direct();
            if (!saved) error = T("保存 USB 直连设置失败");
        }
    } else if (self->access_receiver_selected_) {
        const std::string ssid = lv_textarea_get_text(self->access_host_input_);
        const std::string password = lv_textarea_get_text(self->access_password_input_);
        char existing_ssid[33] = {};
        const bool existing = vibe_wifi::receiver_ssid(existing_ssid, sizeof(existing_ssid));
        if (ssid.empty() || ssid.size() > 32) {
            error = T("热点名称须为 1 到 32 字节");
        } else if (password.empty() && (!existing || ssid != existing_ssid)) {
            error = T("请输入电脑管理页显示的热点密码");
        } else if (!password.empty() && (password.size() < 8 || password.size() > 63)) {
            error = T("热点密码须为 8 到 63 字节");
        } else if (!password.empty() &&
                   vibe_wifi::save_receiver_credentials(ssid.c_str(), password.c_str()) != ESP_OK) {
            error = T("保存接收端热点信息失败");
        } else if (!vibe_pairing::use_receiver()) {
            error = T("保存接收端模式失败");
        } else if (vibe_wifi::connect_receiver() != ESP_OK) {
            error = T("连接接收端热点失败，请核对信息");
        } else {
            saved = true;
        }
    } else if (!self->access_manual_selected_) {
        if (vibe_wifi::receiver_active() || !vibe_wifi::is_connected()) {
            error = T("请先在网络设置连接 Wi-Fi");
        } else {
            saved = vibe_pairing::use_automatic_discovery();
            if (!saved) error = T("保存自动发现设置失败");
        }
    } else {
        std::string host = lv_textarea_get_text(self->access_host_input_);
        bool https = self->access_https_selected_;
        if (host.rfind("https://", 0) == 0) {
            host.erase(0, 8);
            https = true;
        } else if (host.rfind("http://", 0) == 0) {
            host.erase(0, 7);
            https = false;
        }
        while (!host.empty() && host.back() == '/') host.pop_back();
        std::string port_text = lv_textarea_get_text(self->access_port_input_);
        const size_t embedded_port = host.rfind(':');
        if (embedded_port != std::string::npos) {
            port_text = host.substr(embedded_port + 1);
            host.resize(embedded_port);
        }
        char *end = nullptr;
        const unsigned long port = std::strtoul(port_text.c_str(), &end, 10);
        if (port_text.empty() || !end || *end != '\0' || port == 0 || port > 65535) {
            error = T("端口必须是 1 到 65535");
        } else {
            saved = vibe_pairing::set_manual_endpoint(host, static_cast<uint16_t>(port), https, error);
        }
    }
    if (saved && !self->access_receiver_selected_ && vibe_wifi::receiver_active() &&
        vibe_wifi::restore_station() != ESP_OK) {
        if (!self->access_usb_selected_) {
            saved = false;
            error = T("恢复原 Wi-Fi 失败，请重试");
        }
    }
    if (!saved) {
        lv_label_set_text(self->access_status_label_, vibe_i18n::message(error).c_str());
        return;
    }
    vibe_voice::cancel();
    vibe_usb::set_active(self->access_usb_selected_ && self->canOperate());
    self->invalidateCatalog(true);
    {
        std::lock_guard<std::mutex> lock(self->model_mutex_);
        self->model_.sessions.clear();
        self->model_.tasks.clear();
        self->model_.connection_error = self->access_usb_selected_ ? T("正在通过 USB 连接电脑") : self->access_receiver_selected_ ?
            T("正在连接 USB 接收端") : T("正在连接电脑");
        for (auto &provider : self->providers_) provider.selected_session_id.clear();
        ++self->model_.revision;
    }
    self->showSettings(false);
    self->drawn_revision_ = UINT32_MAX;
}

} // namespace esp_brookesia::apps
