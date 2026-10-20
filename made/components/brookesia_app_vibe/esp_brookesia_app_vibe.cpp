// SPDX-License-Identifier: Apache-2.0
// 冒烟测试标准路径开关（唯一源：vibe_usb/include/vibe_smoke_test.h）。
#include "vibe_smoke_test.h"
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_ui_shared.hpp"

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
#include "status_icons.h"
#include "vibe_motion.hpp"
#include "cJSON.h"
#include "bsp/display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_heap_task_info.h"
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

namespace {
constexpr App::Config vibePhoneConfig()
{
    auto config = App::Config::SIMPLE_CONSTRUCTOR(&img_app_vibe, false, false);
    // Vibe owns horizontal assistant swipes and BOOT owns exit. The system's
    // edge gesture mask must not intercept either its buttons or answer area.
    config.flags.enable_navigation_gesture = false;
    return config;
}
} // namespace

namespace esp_brookesia::apps {

VibeCoding *VibeCoding::_instance = nullptr;

VibeCoding *VibeCoding::requestInstance()
{
    if (!_instance) _instance = new VibeCoding();
    return _instance;
}

bool VibeCoding::startOnDisplay()
{
    return requestInstance()->run();
}

VibeCoding::VibeCoding() : App(
    esp_brookesia::systems::base::App::Config::SIMPLE_CONSTRUCTOR("码得", &img_app_vibe, true),
    vibePhoneConfig())
{
    model_.connection_error = "Connecting to bridge";
}

const vibe_provider::Provider &VibeCoding::selectedProviderLocked() const
{
    static const vibe_provider::Provider empty = [] {
        vibe_provider::Provider value;
        value.label = T("暂无助手");
        value.reason = T("请在电脑端添加助手插件");
        return value;
    }();
    const size_t index = static_cast<size_t>(selected_provider_.load());
    return index < providers_.size() ? providers_[index] : empty;
}

vibe_provider::Provider *VibeCoding::findProviderLocked(const std::string &id)
{
    const auto found = std::find_if(providers_.begin(), providers_.end(),
        [&id](const vibe_provider::Provider &provider) { return provider.id == id; });
    return found == providers_.end() ? nullptr : &*found;
}

bool VibeCoding::run()
{
    if (!display_diag_registered_) {
        lv_display_add_event_cb(lv_display_get_default(), displayDiagnostic, LV_EVENT_ALL, this);
        display_diag_registered_ = true;
    }
    vibe_i18n::initialize();
    text_font = &font_puhui_16_4;
    drawer_text_font = &font_puhui_14_4;
    active_ = true;
    exit_requested_ = false;
    invalidateCatalog(true);
    button_epoch_.fetch_add(1);
    rendered_session_id_.clear();
    last_provider_swipe_ms_ = 0;
    last_session_swipe_ms_ = 0;
    drawn_revision_ = UINT32_MAX;
    showing_pairing_layout_ = false;
    // 开机直达助手页：上次运行的浮层状态全部归零，不能带进新的一次 run()。
    settings_open_ = false;
    drawer_open_ = false;
    delete_dialog_open_ = false;
    pairing_drawer_auto_ = false;

    localized_labels.clear();
    last_locale_revision_ = UINT32_MAX;

    // UI 构建按区域拆分（vibe_app_ui.cpp），每个 build 函数只管一个区域；
    // delete_dialog_ 在 buildDrawer 内部后建于 drawer_，保证弹窗层级。
    prepareRunState();
    buildHomeStage();
    buildSettingsHome();
    buildSettingsPanels();
    buildAccessPanel();
    buildNetworkAndEditorPanels();
    buildStatusWidgets();
    buildAnswerArea();
    buildBridgePicker();
    buildDrawer();

    // 开机直达助手页：不再有锁屏，前台连接立即开始。
    openForeground();
    // Brookesia records timers created during run() and removes them on close.
    if (!lv_timer_create(timerCallback, 50, this)) {
        active_ = false;
        ESP_LOGE(BUTTON_TAG, "Cannot start Made UI timer");
        return false;
    }
    render();
    ESP_LOGI(BUTTON_TAG, "Made assistant page ready");

    ensureWorkerStarted();
    bool expected = false;
    if (button_started_.compare_exchange_strong(expected, true)) {
        if (xTaskCreate(buttonEntry, "vibe_boot", 4096, this, 4, nullptr) != pdPASS) {
            button_started_ = false;
            ESP_LOGW(BUTTON_TAG, "Cannot start BOOT button listener");
        }
    }
    return true;
}

bool VibeCoding::back()
{
    if (delete_dialog_open_.load()) {
        showDeleteSessionDialog(false);
        return true;
    }
    if (drawer_open_.load()) {
        showDrawer(false);
        return true;
    }
    if (settings_open_.load()) {
        if (phone_setup_panel_ && !lv_obj_has_flag(phone_setup_panel_, LV_OBJ_FLAG_HIDDEN)) showPhoneSetup(false);
        else if (language_panel_ && !lv_obj_has_flag(language_panel_, LV_OBJ_FLAG_HIDDEN)) showLanguage(false);
        else if (access_editor_panel_ && !lv_obj_has_flag(access_editor_panel_, LV_OBJ_FLAG_HIDDEN)) closeAccessEditor(false);
        else if (wifi_picker_ && !lv_obj_has_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN)) showWifiScan(false);
        else if (receiver_picker_ && !lv_obj_has_flag(receiver_picker_, LV_OBJ_FLAG_HIDDEN)) showReceiverScan(false);
        else if (access_panel_ && !lv_obj_has_flag(access_panel_, LV_OBJ_FLAG_HIDDEN)) showAccess(false);
        else if (volume_panel_ && !lv_obj_has_flag(volume_panel_, LV_OBJ_FLAG_HIDDEN)) showVolume(false);
        else showSettings(false);
        return true;
    }
    return true;
}

bool VibeCoding::pause()
{
    active_ = false;
    phone_setup_requested_ = false;
    phone_setup_epoch_.fetch_add(1);
    vibe_wifi::cancel_receiver_scan();
    vibe_usb::set_active(false);
    vibe_pairing::suspend_foreground();
    restore_station_pending_ = true;
    settings_open_ = false;
    drawer_open_ = false;
    delete_dialog_open_ = false;
    if (settings_overlay_) lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
    if (drawer_) lv_obj_add_flag(drawer_, LV_OBJ_FLAG_HIDDEN);
    if (delete_dialog_) lv_obj_add_flag(delete_dialog_, LV_OBJ_FLAG_HIDDEN);
    button_epoch_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(voice_action_mutex_);
        vibe_voice::cancel();
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        visible_task_id_.clear();
        voice_session_id_.clear();
        pending_create_session_ = false;
        pending_delete_session_ = false;
    }
    return true;
}

bool VibeCoding::resume()
{
    vibe_i18n::initialize();
    active_ = true;
    openForeground();
    invalidateCatalog(true);
    button_epoch_.fetch_add(1);
    active_ = true;
    exit_requested_ = false;
    drawn_revision_ = UINT32_MAX;
    return true;
}

bool VibeCoding::close()
{
    active_ = false;
    phone_setup_requested_ = false;
    phone_setup_epoch_.fetch_add(1);
    vibe_wifi::cancel_receiver_scan();
    vibe_usb::set_active(false);
    vibe_pairing::suspend_foreground();
    restore_station_pending_ = true;
    settings_open_ = false;
    delete_dialog_open_ = false;
    exit_requested_ = false;
    button_epoch_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(voice_action_mutex_);
        vibe_voice::cancel();
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        visible_task_id_.clear();
        voice_session_id_.clear();
        pending_create_session_ = false;
        pending_delete_session_ = false;
    }
    localized_labels.clear();
    sprite_stage_ = nullptr;
    for (auto &slot : sides_) {
        slot.stage = nullptr;
        slot.provider.clear();
        slot.version = 0;
        slot.side = 0;
    }
    prefetch_provider_.clear();
    prefetch_version_ = 0;
    prefetch_slot_ = -1;
    artwork_applied_.clear();
    artwork_applied_version_ = 0;
    artwork_seen_provider_.clear();
    artwork_dirty_.store(false);
    artwork_request_.store(false);
    {
        std::lock_guard<std::mutex> lock(artwork_mutex_);
        artwork_attempted_ = false;  // 重新进入后允许再校验一轮缓存
        cached_apply_provider_.clear();  // 缓存直换的记忆一并作废
        cached_apply_version_ = 0;
        cached_apply_attempts_ = 0;
    }
    drawer_ = nullptr;
    drawer_grip_ = nullptr;
    language_panel_ = nullptr;
    language_zh_button_ = nullptr;
    language_en_button_ = nullptr;
    language_auto_button_ = nullptr;
    language_hint_ = nullptr;
    theme_panel_ = nullptr;
    theme_rows_ = nullptr;
    theme_status_label_ = nullptr;
    power_panel_ = nullptr;
    firmware_panel_ = nullptr;
    firmware_status_label_ = nullptr;
    power_timeout_button_ = nullptr;
    power_timeout_label_ = nullptr;
    volume_panel_ = nullptr;
    volume_value_label_ = nullptr;
    power_off_overlay_ = nullptr;
    provider_label_ = nullptr;
    session_title_tap_ = nullptr;
    rendered_catalog_ready_ = false;
    page_label_ = nullptr;
    new_session_button_ = nullptr;
    delete_session_button_ = nullptr;
    delete_dialog_ = nullptr;
    delete_dialog_title_ = nullptr;
    settings_button_ = nullptr;
    settings_overlay_ = nullptr;
    settings_home_ = nullptr;
    access_panel_ = nullptr;
    access_auto_button_ = nullptr;
    access_manual_button_ = nullptr;
    access_receiver_button_ = nullptr;
    access_usb_button_ = nullptr;
    access_usb_hint_ = nullptr;
    access_host_input_ = nullptr;
    access_port_input_ = nullptr;
    access_password_input_ = nullptr;
    access_https_button_ = nullptr;
    access_https_label_ = nullptr;
    access_keyboard_ = nullptr;
    access_editor_panel_ = nullptr;
    access_editor_input_ = nullptr;
    access_editor_title_ = nullptr;
    access_editor_target_ = nullptr;
    access_status_label_ = nullptr;
    receiver_scan_button_ = nullptr;
    receiver_picker_ = nullptr;
    receiver_list_ = nullptr;
    receiver_hint_ = nullptr;
    receiver_refresh_button_ = nullptr;
    receiver_rows_.clear();
    receiver_pressed_button_ = nullptr;
    receiver_scan_revision_ = UINT32_MAX;
    access_save_button_ = nullptr;
    access_change_button_ = nullptr;
    access_change_label_ = nullptr;
    access_picker_hint_ = nullptr;
    phone_setup_button_ = nullptr;
    phone_setup_panel_ = nullptr;
    phone_setup_qr_ = nullptr;
    phone_setup_hint_ = nullptr;
    phone_setup_network_ = nullptr;
    phone_setup_password_ = nullptr;
    phone_setup_address_ = nullptr;
    bridge_picker_ = nullptr;
    bridge_list_ = nullptr;
    bridge_hint_ = nullptr;
    bridge_refresh_button_ = nullptr;
    bridge_rows_.clear();
    bridge_list_revision_ = UINT32_MAX;
    bridge_pressed_button_ = nullptr;
    position_label_ = nullptr;
    meta_label_ = nullptr;
    instruction_label_ = nullptr;
    answer_area_ = nullptr;
    result_label_ = nullptr;
    answer_scope_.clear();
    answer_text_.clear();
    showing_pairing_layout_ = false;
    displayed_task_id_.clear();
    rendered_session_id_.clear();
    last_provider_swipe_ms_ = 0;
    last_session_swipe_ms_ = 0;
    return true;
}

void VibeCoding::buttonEntry(void *arg)
{
    static_cast<VibeCoding *>(arg)->buttonLoop();
}

void VibeCoding::buttonLoop()
{
    BootClickGesture gesture;
    bool configured_for_foreground = false;
    uint32_t seen_epoch = button_epoch_.load();
    int seen_provider = selected_provider_.load();
    std::string armed_task_id;
    int armed_provider = -1;
    while (true) {
        boot_tick_ms_.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
        const uint32_t current_epoch = button_epoch_.load();
        const int current_provider = selected_provider_.load();
        if (!active_.load() || current_epoch != seen_epoch || current_provider != seen_provider) {
            gesture.reset();
            armed_task_id.clear();
            armed_provider = -1;
            configured_for_foreground = false;
            seen_epoch = current_epoch;
            seen_provider = current_provider;
        }
        if (!active_.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (exit_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (!configured_for_foreground) {
            gpio_config_t config = {};
            config.pin_bit_mask = 1ULL << GPIO_NUM_0;
            config.mode = GPIO_MODE_INPUT;
            config.pull_up_en = GPIO_PULLUP_ENABLE;
            config.pull_down_en = GPIO_PULLDOWN_DISABLE;
            config.intr_type = GPIO_INTR_DISABLE;
            const esp_err_t result = gpio_config(&config);
            if (result != ESP_OK) {
                ESP_LOGW(BUTTON_TAG, "BOOT input unavailable: %s", esp_err_to_name(result));
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            configured_for_foreground = true;
            gesture.reset();
        }

        const bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        // hold 是否武装：voice 可用且不在设置/删除弹窗时，长按归说话；
        // 否则长按无动作（Home 分支已按用户要求移除）。
        gesture.setHoldArmed(voice_available_.load() &&
                             !settings_open_.load() && !delete_dialog_open_.load());
        const bool had_sequence = gesture.sequenceActive();
        const auto decision = gesture.update(pressed, esp_timer_get_time() / 1000);
        if (!had_sequence && gesture.sequenceActive()) {
            std::lock_guard<std::mutex> lock(model_mutex_);
            armed_task_id = visible_task_id_;
            armed_provider = selected_provider_.load();
        }
        if (decision != BootClickGesture::Decision::None) {
            if (active_.load() && current_epoch == button_epoch_.load() &&
                armed_provider == selected_provider_.load()) {
                if (settings_open_.load() || delete_dialog_open_.load()) {
                    // The settings keyboard owns touch input; no task action
                    // should be triggered by a stray BOOT click while editing.
                } else if (decision == BootClickGesture::Decision::Hold) {
                    // 长按说话：按住开始录音，松手（Release）立即发送。
                    startVoice(/*hold_mode=*/true);
                } else if (decision == BootClickGesture::Decision::Release) {
                    {
                        std::lock_guard<std::mutex> lock(voice_action_mutex_);
                        vibe_voice::finish();
                    }
                } else if (decision == BootClickGesture::Decision::Voice) {
                    // 单击不再录音（长按说话是主交互），但保留配对重试与
                    // 各类提示；配对/配置正常时只提示新交互方式。
                    startVoice(/*hold_mode=*/false, /*allow_record=*/false);
                } else {
                    std::lock_guard<std::mutex> lock(voice_action_mutex_);
                    const auto voice = vibe_voice::status();
                    const bool voice_in_progress = voice.phase == vibe_voice::Phase::Recording ||
                                                   voice.phase == vibe_voice::Phase::Uploading;
                    bool voice_for_selected_session = false;
                    {
                        std::lock_guard<std::mutex> model_lock(model_mutex_);
                        voice_for_selected_session = !voice_session_id_.empty() &&
                            voice_session_id_ == selectedProviderLocked().selected_session_id;
                    }
                    const bool new_voice_task_not_visible = voice.phase == vibe_voice::Phase::Submitted &&
                        voice_for_selected_session && !voice.task_id.empty() && !submitted_task_seen_.load();
                    if (decision == BootClickGesture::Decision::Cancel && voice_in_progress) {
                        vibe_voice::cancel();
                    } else if (new_voice_task_not_visible) {
                        // The screen may still hold the previous job until its next poll.
                        // A triple-click cancels the freshly submitted voice job by its
                        // returned ID; a double-click waits until that job is displayed.
                        if (decision == BootClickGesture::Decision::Cancel) {
                            queueSubmittedVoiceCancel(voice.task_id);
                        }
                    } else if (!voice_in_progress && !armed_task_id.empty()) {
                        queueActionForId(decision == BootClickGesture::Decision::Confirm ? "confirm" : "cancel",
                                         armed_task_id);
                    }
                }
                if (!settings_open_.load() && !delete_dialog_open_.load()) {
                    const char *board_event = nullptr;
                    if (decision == BootClickGesture::Decision::Voice) board_event = "boot.click";
                    else if (decision == BootClickGesture::Decision::Confirm) board_event = "boot.double";
                    else if (decision == BootClickGesture::Decision::Cancel) board_event = "boot.triple";
                    if (board_event) queueBoardEvent(board_event);
                }
            }
            // 决策处理完不能立刻清 armed_provider：Hold 与 Release 属于同一
            // 按压序列，Hold 处理完就清的话，松手的 Release 会因
            // armed_provider(-1) != selected 被外层守卫吞掉，录音永远等不到
            // finish() 只能跑满 30s 上限（2026-10-17 实机实测踩坑）。
            // 只在按压序列彻底结束后再清。
            if (!gesture.sequenceActive()) {
                armed_task_id.clear();
                armed_provider = -1;
            }
        } else if (!gesture.sequenceActive()) {
            armed_task_id.clear();
            armed_provider = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void VibeCoding::displayDiagnostic(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    uint32_t stage = 0;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_REFR_START: stage = 1; break;
    case LV_EVENT_RENDER_START: stage = 2; break;
    case LV_EVENT_RENDER_READY: stage = 3; break;
    case LV_EVENT_FLUSH_START: stage = 4; break;
    case LV_EVENT_FLUSH_FINISH: stage = 5; break;
    case LV_EVENT_FLUSH_WAIT_START: stage = 6; break;
    case LV_EVENT_FLUSH_WAIT_FINISH: stage = 7; break;
    case LV_EVENT_REFR_READY: stage = 8; break;
    default: return;
    }
    self->display_stage_.store(stage);
    self->display_count_.fetch_add(1);
}

void VibeCoding::timerCallback(lv_timer_t *timer)
{
    auto *self = static_cast<VibeCoding *>(timer->user_data);
    if (!self || !self->active_.load()) return;
    self->ui_tick_ms_.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    self->ui_stage_.store(1);
    if (self->exit_requested_.load()) {
        self->exit_requested_.store(false);
    } else {
        self->applyRemoteCommands();
        self->render();
    }
#if VIBE_SMOKE_TEST
    self->smokeTick();
#endif
    self->ui_stage_.store(100);
}

#if VIBE_SMOKE_TEST
// 冒烟测试：开机 3s（LVGL 起来）后每 200ms 自动随机左右滑（esp_random
// 掷硬币换向，覆盖双向净位移为零的死角）——模拟真实
// 用户「滑溜」连续快滑，不等素材拉齐（未就绪时 handleUiSwipe 内部自然
// 空转），走 handleUiSwipe→chooseProvider 的真实切页路径（预取晋升/缓存
// 直换/worker 老路全都在循环里），每次打一行 PSRAM 水位。这是内存问题
// 的闭环复现器：free 单调下滑=泄漏，largest 远低于 free=碎片，wm 递降=
// 累积恶化。
void VibeCoding::smokeTick()
{
    const uint32_t now = lv_tick_get();
    if (burnin_next_ms_ == 0) {
        burnin_next_ms_ = now + 3000;
        ESP_LOGW(BUTTON_TAG, "[burnin] start in 3s: auto swipe left/right every 200ms");
        return;
    }
    if (now < burnin_next_ms_) return;
    burnin_next_ms_ = now + 200;
    burnin_ticks_.fetch_add(1);
    if (!canOperate() || settings_open_.load() || drawer_open_.load()) return;
    const int before = selected_provider_.load();
    // 随机方向：固定交替净位移为零，只能在两个邻居间弹跳（用户实测指出）；
    // 硬件随机数让四个助手都能被访问，连续同向连滑的序列也更像真实用户。
    burnin_left_next_ = (esp_random() & 1) != 0;
    handleUiSwipe(burnin_left_next_ ? "left" : "right");
    if (selected_provider_.load() == before) return;  // 目录未就绪，空转不计次
    ++burnin_count_;
    const unsigned free_kb = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    const unsigned largest_kb = static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);
    const unsigned wm_kb = static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);
    ESP_LOGW(BUTTON_TAG,
        "[burnin] #%d %s free=%uKB largest=%uKB wm=%uKB",
        burnin_count_, burnin_left_next_ ? "left" : "right", free_kb, largest_kb, wm_kb);
    // 水位经事件通道直发桥接器（观察期串口归桥接器连接；未订阅事件桥接器
    // 只打日志后拒收，无副作用）。字段 ≤120 字节协议上限内。
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "#%d %s free=%uKB largest=%uKB wm=%uKB",
                 burnin_count_, burnin_left_next_ ? "left" : "right", free_kb, largest_kb, wm_kb);
        std::lock_guard<std::mutex> lock(model_mutex_);
        burnin_event_body_ = std::string("{\"name\":\"burnin.mem\",\"fields\":{\"msg\":\"") + msg + "\"}}";
    }
}
#endif

void VibeCoding::openForeground()
{
    vibe_pairing::begin_foreground_connection();
    foreground_network_pending_.store(true);
    drawn_revision_ = UINT32_MAX;
}

void VibeCoding::providerSwipeCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate() || self->settings_open_.load()) return;
    // 同一次物理滑动只允许一条路径裁决：跟手拖动（homeDragCallback）一旦
    // 激活（拖过 6px 就激活，远早于 LVGL 手势的 50px 阈值），本次按住的
    // 横向手势就归它管——LVGL 的 GESTURE 事件在手指还按着时就会发出，
    // 这里若再切一次，长距离慢滑必然「一下划两屏」。松手时由拖动距离
    // 统一裁决（见 homeDragCallback 的 RELEASED 分支）。
    if (self->home_dragging_) return;
    // 跟手拖动已经处理过这次横向手势，避免松手时重复切助手。
    if (self->drag_end_ms_ != 0 && lv_tick_get() - self->drag_end_ms_ < 400) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) return;
    const uint32_t now = lv_tick_get();
    if (self->last_provider_swipe_ms_ != 0 && now - self->last_provider_swipe_ms_ < 350) return;
    self->last_provider_swipe_ms_ = now;
    // 任务结果页（抽屉）里横向手势切当前助手的不同任务；主屏才切助手。
    if (self->drawer_open_.load()) self->chooseSession(direction == LV_DIR_LEFT ? 1 : -1);
    else self->chooseProvider(direction == LV_DIR_LEFT ? 1 : -1);
}

void VibeCoding::chooseProvider(int direction)
{
    choose_calls_.fetch_add(1);
    const auto voice = vibe_voice::status();
    if (voice.phase == vibe_voice::Phase::Recording ||
        voice.phase == vibe_voice::Phase::Uploading) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.voice_hint = "Finish voice capture before switching agent";
        ++model_.revision;
        return;
    }
    submitted_task_seen_ = true;
    displayed_task_id_.clear();
    rendered_session_id_.clear();
    std::string chosen;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (!config_loaded_.load() || providers_.empty()) return;
        const int count = static_cast<int>(providers_.size());
        const int current = selected_provider_.load();
        selected_provider_.store((current + direction + count) % count);
        const auto &entry = providers_[static_cast<size_t>(selected_provider_.load())];
        if (entry.sprite.valid()) chosen = entry.id;
        button_epoch_.fetch_add(1);
        model_.sessions.clear();
        model_.tasks.clear();
        model_.feedback.clear();
        model_.feedback_task_id.clear();
        model_.voice_hint.clear();
        model_.session_notice.clear();
        model_.connection_error = "Loading sessions...";
        visible_task_id_.clear();
        ++model_.revision;
    }
    drawn_revision_ = UINT32_MAX;
    // 侧舞台已挂好（或可立即挂上）目标素材就晋升为当前舞台；未命中由
    // render→maybeApplyArtwork 走缓存直换/worker 老路。
    if (!chosen.empty()) tryPromotePrefetch(chosen);
    render();
}

void VibeCoding::sessionSwipeCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate() || self->settings_open_.load()) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction != LV_DIR_TOP && direction != LV_DIR_BOTTOM) return;
    const uint32_t now = lv_tick_get();
    if (self->last_session_swipe_ms_ != 0 && now - self->last_session_swipe_ms_ < 350) return;
    self->last_session_swipe_ms_ = now;
    self->chooseSession(direction == LV_DIR_TOP ? 1 : -1);
}

// 横向翻屏跟手：按下记录起点；拖动时整体平移 screen（素材是子对象，
// 随屏一起走）；松手按位置裁决——拖过半屏切到相邻助手，不满半屏弹回。
void VibeCoding::homeDragCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *screen = lv_scr_act();

    // 松手/手势被打断（滑出屏幕、弹窗抢走触摸等都会发 PRESS_LOST）：
    // 无论当时系统处于什么状态，先把整屏归位——否则一次被截断的拖动
    // 会把屏幕永久卡在偏移处（偏一半=素材贴边，偏一屏=整页空白）。
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!self->home_dragging_) return;
        self->home_dragging_ = false;
        self->last_home_tap_ms_ = 0; // 拖动不是点击：清掉双击计数，避免拖动松手后的短按被误判成双击第二击
        const int w = made_screen_w();
        const int dx = self->drag_dx_;
        self->drag_end_ms_ = lv_tick_get();
        bool switched = false;
        if (code == LV_EVENT_RELEASED && !self->drawer_open_.load() &&
            std::abs(dx) > w * 0.5) {
            // 松手时哪一屏占住屏幕超过一半就切到哪一屏：拖过半屏（|dx|>w/2）
            // 说明目标屏已占据大半显示区，切过去；否则弹回当前屏。
            // 拖动激活期间手势路径已让位（providerSwipeCallback 的
            // home_dragging_ 门），这里是唯一的切换裁决点。350ms 窗口只
            // 兜底极端快滑：采样点一帧跨过 50px 时 GESTURE 可能在拖动
            // 激活前抢先发出并已切换。
            const uint32_t now = lv_tick_get();
            if (self->last_provider_swipe_ms_ == 0 ||
                now - self->last_provider_swipe_ms_ >= 350) {
                self->last_provider_swipe_ms_ = now;
                self->chooseProvider(dx < 0 ? 1 : -1);
                switched = true;
            }
        }
        // 没切过去（弹回当前屏）：侧舞台素材原地保留——下次同向滑动直接
        // 滑入视野，省一次解码；目标变化时由 maybeStartPrefetch 负责撤换。
        // 位置归零（切助手后 render 会重建内容，原位即可）。
        lv_obj_set_x(screen, self->screen_base_x_);
        // 切助手已设置 last_provider_swipe_ms_ 抑制重复；这里统一再保一次。
        if (switched) self->last_provider_swipe_ms_ = lv_tick_get();
        return;
    }

    if (!self->canOperate() || self->settings_open_.load() || self->drawer_open_.load())
        return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        self->home_dragging_ = false;
        self->drag_dx_ = 0;
        self->drag_start_x_ = p.x;
        self->drag_start_y_ = p.y;
        self->screen_base_x_ = lv_obj_get_x(screen);
        self->last_pressing_ms_ = lv_tick_get();
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        self->last_pressing_ms_ = lv_tick_get();  // 按压心跳：给 render() 的看门狗用
        const int dx = p.x - self->drag_start_x_;
        const int dy = p.y - self->drag_start_y_;
        if (!self->home_dragging_) {
            if (std::abs(dx) < 6 && std::abs(dy) < 6) return;
            // 纵向为主的手势交给 sessionSwipe，不抢。
            if (std::abs(dy) > std::abs(dx)) { self->home_dragging_ = false; return; }
            self->home_dragging_ = true;
        }
        const int w = made_screen_w();
        const int clamped = std::max(-w, std::min(w, dx));
        self->drag_dx_ = clamped;
        lv_obj_set_x(screen, self->screen_base_x_ + clamped);
        // 滑动方向已明确：确保该方向侧台就绪（三舞台轮换常态下早已就绪，
        // 这里零开销）。每拍调用安全。
        self->maybeStartPrefetch(clamped < 0 ? 1 : (clamped > 0 ? -1 : 0));
        // 素材是 screen 的子对象，随屏幕整体平移：用户要求滑动时素材跟手一起走。
        return;
    }
}

// 从文本里抽出 URL，按扩展名分 image / link 两类附件。
std::vector<VibeCoding::Attachment> VibeCoding::parseUrlAttachments(const std::string &text)
{
    std::vector<Attachment> out;
    static const char *kImgExt[] = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"};
    const size_t n = text.size();
    for (size_t i = 0; i + 4 < n; ++i) {
        if ((text[i] == 'h' && text.compare(i, 7, "http://") == 0) ||
            (text[i] == 'h' && text.compare(i, 8, "https://") == 0)) {
            size_t j = i;
            while (j < n && !isspace((unsigned char)text[j]) && text[j] != '"' &&
                   text[j] != '`' && text[j] != '<' && text[j] != '>')
                ++j;
            std::string url = text.substr(i, j - i);
            // 去掉常见的句末标点
            while (!url.empty() && (url.back() == '.' || url.back() == ',' || url.back() == ')' ||
                                    url.back() == ']' || url.back() == ';'))
                url.pop_back();
            if (url.size() < 8) { i = j; continue; }
            Attachment a;
            a.url = url;
            a.type = "link";
            std::string lower = url;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            for (const char *ext : kImgExt) {
                if (lower.size() > strlen(ext) &&
                    lower.compare(lower.size() - strlen(ext), strlen(ext), ext) == 0) {
                    a.type = "image";
                    break;
                }
            }
            out.push_back(std::move(a));
            i = j;
        }
    }
    return out;
}

// 从 job JSON 的 attachments 数组解析（桥接器显式下发）。
std::vector<VibeCoding::Attachment> VibeCoding::parseAttachments(const cJSON *job)
{
    std::vector<VibeCoding::Attachment> out;
    if (!job) return out;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(job, "attachments");
    if (!cJSON_IsArray(arr)) return out;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) continue;
        Attachment a;
        a.type = jsonString(item, "type");
        a.url = jsonString(item, "url");
        a.text = jsonString(item, "text");
        if (a.url.empty()) continue;
        if (a.type != "image" && a.type != "link") a.type = "link";
        out.push_back(std::move(a));
    }
    return out;
}

void VibeCoding::chooseSession(int direction)
{
    const auto voice = vibe_voice::status();
    if (voice.phase == vibe_voice::Phase::Recording ||
        voice.phase == vibe_voice::Phase::Uploading) return;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto &sessions = model_.sessions;
        if (!config_loaded_.load() || sessions.empty()) return;
        const auto &selected_id = selectedProviderLocked().selected_session_id;
        auto current = std::find_if(sessions.begin(), sessions.end(), [&selected_id](const Session &session) {
            return session.id == selected_id;
        });
        const int count = static_cast<int>(sessions.size());
        const int index = current == sessions.end() ? 0 : static_cast<int>(current - sessions.begin());
        auto *provider = findProviderLocked(selectedProviderLocked().id);
        if (!provider) return;
        provider->selected_session_id = sessions[(index + direction + count) % count].id;
        model_.tasks.clear();
        model_.feedback.clear();
        model_.feedback_task_id.clear();
        model_.session_notice.clear();
        visible_task_id_.clear();
        ++model_.revision;
    }
    button_epoch_.fetch_add(1);
    submitted_task_seen_ = true;
    displayed_task_id_.clear();
    rendered_session_id_.clear();
    drawn_revision_ = UINT32_MAX;
    render();
}

void VibeCoding::newSessionCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self && self->canOperate()) self->requestNewSession();
}

void VibeCoding::showDeleteSessionDialog(bool visible)
{
    if (!delete_dialog_) return;
    delete_dialog_open_ = visible;
    button_epoch_.fetch_add(1);
    if (visible) {
        lv_obj_remove_flag(delete_dialog_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(delete_dialog_);
    } else {
        lv_obj_add_flag(delete_dialog_, LV_OBJ_FLAG_HIDDEN);
        delete_dialog_provider_id_.clear();
        delete_dialog_session_id_.clear();
    }
}

void VibeCoding::showDrawer(bool visible)
{
    if (!drawer_) return;
    drawer_open_ = visible;
    button_epoch_.fetch_add(1);
    if (visible) {
        lv_obj_remove_flag(drawer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(drawer_);
        // 打开抽屉即算读过回显：铃铛当场熄灭，不寄望下一次 render。
        unread_reply_ = false;
        if (unread_mark_) lv_obj_add_flag(unread_mark_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(drawer_, LV_OBJ_FLAG_HIDDEN);
    }
}

void VibeCoding::drawerSwipeCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate() || self->settings_open_.load()) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    // 主页上滑拉出抽屉；抽屉内下拉收起（同一回调挂在两个对象上）。
    if (direction == LV_DIR_TOP && !self->drawer_open_.load()) {
        // 斜滑可能先激活了翻屏拖动（横向分量先过 6px 阈值），随后手势判定
        // 为上滑拉抽屉：必须立即终止拖动并归位，否则拖动标志悬着、screen
        // 卡在偏移处（松手事件若再丢，看门狗也要再等 400ms）。
        if (self->home_dragging_) {
            self->home_dragging_ = false;
            lv_obj_set_x(lv_scr_act(), 0);
        }
        self->showDrawer(true);
    }
    else if (direction == LV_DIR_BOTTOM && self->drawer_open_.load()) self->showDrawer(false);
}

void VibeCoding::deleteSessionCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate()) return;
    const auto voice = vibe_voice::status();
    std::string title;
    {
        std::lock_guard<std::mutex> lock(self->model_mutex_);
        const auto &provider = self->selectedProviderLocked();
        const auto found = std::find_if(self->model_.sessions.begin(), self->model_.sessions.end(),
            [&provider](const Session &session) { return session.id == provider.selected_session_id; });
        if (found == self->model_.sessions.end() || self->pending_delete_session_ ||
            self->delete_session_in_flight_) return;
        if (voice.phase == vibe_voice::Phase::Recording || voice.phase == vibe_voice::Phase::Uploading) {
            self->model_.session_notice = T("录音中，请稍后删除任务");
            ++self->model_.revision;
            return;
        }
        self->delete_dialog_provider_id_ = provider.id;
        self->delete_dialog_session_id_ = found->id;
        title = found->title.empty() ? T("未命名任务") : found->title;
    }
    lv_label_set_text(self->delete_dialog_title_, title.c_str());
    self->showDeleteSessionDialog(true);
}

void VibeCoding::deleteSessionConfirmCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate() || !self->delete_dialog_open_.load()) return;
    {
        std::lock_guard<std::mutex> lock(self->model_mutex_);
        if (self->selectedProviderLocked().id == self->delete_dialog_provider_id_ &&
            self->selectedProviderLocked().selected_session_id == self->delete_dialog_session_id_ &&
            !self->pending_delete_session_ && !self->delete_session_in_flight_) {
            self->pending_delete_provider_id_ = self->delete_dialog_provider_id_;
            self->pending_delete_session_id_ = self->delete_dialog_session_id_;
            self->pending_delete_session_ = true;
            self->model_.session_notice = T("正在删除任务…");
            ++self->model_.revision;
        }
    }
    self->showDeleteSessionDialog(false);
}

void VibeCoding::deleteSessionCancelCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showDeleteSessionDialog(false);
}

void VibeCoding::sessionTitleTapCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self && self->canOperate()) self->chooseSession(1);
}

void VibeCoding::languageCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate()) return;
    const auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const bool saved = target == self->language_auto_button_ ? vibe_i18n::set_follow_bridge() :
        vibe_i18n::set_locale(target == self->language_en_button_ ? "en" : "zh-CN");
    if (!saved) {
        lv_label_set_text(self->language_hint_, T("语言保存失败，请重试"));
        return;
    }
    // Let the worker re-read localized built-in status without changing sessions.
    self->language_refresh_pending_.store(true);
    self->refreshLanguage();
    self->render();
}

void VibeCoding::refreshLanguage()
{
    const uint32_t locale_revision = vibe_i18n::revision();
    if (locale_revision == last_locale_revision_) return;
    last_locale_revision_ = locale_revision;
    for (const auto &binding : localized_labels)
        lv_label_set_text(binding.object, T(binding.key.c_str()));
    lv_textarea_set_placeholder_text(access_port_input_, T("端口"));
    lv_textarea_set_placeholder_text(access_password_input_, T("密码（留空沿用已保存的）"));
    updateAccessModeButtons();
    bridge_list_revision_ = UINT32_MAX;
    receiver_scan_revision_ = UINT32_MAX;
    drawn_revision_ = UINT32_MAX;
    const bool automatic = vibe_i18n::follows_bridge();
    const bool english = vibe_i18n::locale() == vibe_i18n::Locale::English;
    // 选中态：accent 实底 + text_on_accent；未选中回到 ghost（透明底）。
    auto setSegmentSelected = [](lv_obj_t *button, bool selected) {
        if (!button) return;
        lv_obj_set_style_bg_color(button, lv_color_hex(selected ? VT(accent) : VT(btn_normal)), 0);
        lv_obj_set_style_bg_opa(button, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (lv_obj_get_child_cnt(button) > 0) {
            lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                        lv_color_hex(selected ? VT(text_on_accent) : VT(text_primary)), 0);
        }
    };
    setSegmentSelected(language_zh_button_, !automatic && !english);
    setSegmentSelected(language_en_button_, !automatic && english);
    setSegmentSelected(language_auto_button_, automatic);
    lv_label_set_text(language_hint_, automatic ? T("离线沿用上次语言") : T("设备语言独立保存"));
}

void VibeCoding::settingsCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self && self->canOperate()) self->showSettings(true);
}

void VibeCoding::settingsCloseCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showSettings(false);
}

void VibeCoding::languageOpenCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showLanguage(true);
}

void VibeCoding::languageBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showLanguage(false);
}

// ---- 电源设置：空闲自动熄屏（只控背光，业务逻辑不变） ----

void VibeCoding::requestNewSession()
{
    if (!canOperate()) return;
    const auto voice = vibe_voice::status();
    const auto pairing = vibe_pairing::snapshot();
    std::lock_guard<std::mutex> lock(model_mutex_);
    const auto &provider = selectedProviderLocked();
    if (voice.phase == vibe_voice::Phase::Recording || voice.phase == vibe_voice::Phase::Uploading) {
        model_.session_notice = T("录音中，请稍后新建任务");
    } else if (pairing.phase != vibe_pairing::Phase::Paired ||
               !vibe_pairing::authorized_for_foreground()) {
        model_.session_notice = T("请先连接电脑");
    } else if (!config_loaded_.load()) {
        model_.session_notice = T("正在读取电脑设置");
    } else if (!provider.available) {
        model_.session_notice = T("请先在电脑端启用此助手");
    } else if (!validProjectId(voice_project_id_)) {
        model_.session_notice = T("请先在电脑端选择默认项目");
    } else if (!pending_create_session_ && !create_session_in_flight_) {
        pending_create_session_ = true;
        pending_create_provider_id_ = provider.id;
        pending_create_project_id_ = voice_project_id_;
        model_.session_notice = T("正在创建任务…");
    } else {
        return;
    }
    ++model_.revision;
}

void VibeCoding::startVoice(bool hold_mode, bool allow_record)
{
    std::lock_guard<std::mutex> voice_lock(voice_action_mutex_);
    if (!canOperate()) return;
    const auto voice = vibe_voice::status();
    if (voice.phase == vibe_voice::Phase::Recording ||
        voice.phase == vibe_voice::Phase::Uploading) return;

    std::string hint;
    std::string started_session_id;
    bool started = false;
    const auto pairing = vibe_pairing::snapshot();
    if (pairing.phase != vibe_pairing::Phase::Paired ||
        !vibe_pairing::authorized_for_foreground()) {
        if (pairing.retry_required) {
            vibe_pairing::retry();
            return;
        }
        hint = "Connect to computer first";
    } else if (pairing.url.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) {
        hint = T("正在同步时间，以安全连接电脑");
    } else if (!config_loaded_.load()) {
        hint = "Waiting for bridge settings";
    } else if (!voice_available_.load()) {
        hint = T("请在电脑工作台配置语音识别");
    } else {
        std::string project_id;
        std::string session_id;
        std::string provider_id;
        bool provider_ready = true;
        bool instruction_busy = false;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            const auto &provider = selectedProviderLocked();
            provider_ready = provider.available;
            provider_id = provider.id;
            const auto &selected_id = provider.selected_session_id;
            const auto session = std::find_if(model_.sessions.begin(), model_.sessions.end(),
                [&selected_id](const Session &item) { return item.id == selected_id; });
            if (session != model_.sessions.end()) {
                session_id = session->id;
                project_id = session->project_id;
            }
            // 列表只保留当前任务最新一条。待确认、排队、运行中时桥接器会在
            // 识别完成后再拒绝，这里提前拦住，避免白录和白上传。
            if (!model_.tasks.empty() && model_.tasks.front().session_id == selected_id) {
                const std::string &status = model_.tasks.front().status;
                instruction_busy = status == "waiting_confirmation" ||
                                   status == "queued" || status == "running";
            }
        }
        if (!provider_ready) {
            hint = "Selected coding agent is unavailable";
        } else if (session_id.empty()) {
            hint = T("点右上角 + 创建任务");
        } else if (project_id.empty()) {
            hint = T("当前任务没有项目，请在电脑端设置");
        } else if (instruction_busy) {
            hint = T("请先完成或取消上一条指令");
        } else if (!allow_record) {
            // 单击路径：一切就绪也不录音，引导长按说话（录音仅由 Hold 触发
            // 或远程 voice.start 发起）。
            hint = T("请按住 BOOT 说话，松手即发送");
        } else {
            reclaimSpriteMemoryForVoice();  // PSRAM 紧张时让侧台/预取先腾地方
            if (vibe_voice::start(pairing.url, pairing.token,
                                  provider_id, project_id, session_id, hold_mode)) {
                submitted_task_seen_ = false;
                started = true;
                started_session_id = session_id;
            } else {
                hint = vibe_voice::status().message;
                if (hint.empty()) hint = "Microphone unavailable";
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (started) {
            model_.session_notice.clear();
            voice_session_id_ = std::move(started_session_id);
        }
        model_.voice_hint = std::move(hint);
        ++model_.revision;
    }
}

void VibeCoding::queueActionForId(const char *verb, const std::string &task_id)
{
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (!canOperate() || model_.action_in_flight || !validTaskId(task_id) ||
        task_id != visible_task_id_) return;
    auto task_it = std::find_if(model_.tasks.begin(), model_.tasks.end(), [&task_id](const Task &task) {
        return task.id == task_id;
    });
    if (task_it == model_.tasks.end()) return;
    const Task &task = *task_it;
    const auto &provider = selectedProviderLocked();
    if (!config_loaded_.load() || task.provider != provider.id || task.session_id != provider.selected_session_id) return;
    if (std::string(verb) == "cancel" && task.status == "running" && !provider.cancel) {
        model_.feedback_external = false;
        model_.feedback = T("此助手不支持停止执行，请在电脑端操作");
        model_.feedback_task_id = task.id;
        ++model_.revision;
        return;
    }
    if (std::string(verb) == "confirm" && task.status != "waiting_confirmation") return;
    if (std::string(verb) == "confirm" && task.instruction_truncated) {
        // Full text is unavailable on the device, so confirm on the PC.
        model_.feedback_external = false;
        model_.feedback = T("任务过长，请在电脑任务页核对并确认");
        model_.feedback_task_id = task.id;
        ++model_.revision;
        return;
    }
    if (std::string(verb) == "cancel" && task.status != "waiting_confirmation" &&
        task.status != "queued" && task.status != "running") return;
    pending_action_ = {task.id, verb};
    model_.action_in_flight = true;
    model_.feedback_external = false;
    model_.feedback = std::string(std::string(verb) == "confirm" ? "Confirming" : "Cancelling") + "...";
    model_.feedback_task_id = task.id;
    ++model_.revision;
}

void VibeCoding::queueSubmittedVoiceCancel(const std::string &task_id)
{
    if (!validTaskId(task_id)) return;
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (!canOperate() || model_.action_in_flight) return;
    pending_action_ = {task_id, "cancel"};
    model_.action_in_flight = true;
    model_.feedback_external = false;
    model_.feedback = "Cancelling new voice task...";
    model_.feedback_task_id = task_id;
    ++model_.revision;
}

void VibeCoding::bridgeRefreshCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate()) return;
    vibe_pairing::scan_bridges();
}

void VibeCoding::bridgeSelectCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate()) return;
    auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_indev_t *input = lv_indev_get_act();
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        self->bridge_pressed_button_ = target;
        self->bridge_press_moved_ = false;
        if (input) lv_indev_get_point(input, &self->bridge_press_point_);
        return;
    }
    if (self->bridge_pressed_button_ != target) return;
    if (input) {
        lv_point_t point{};
        lv_indev_get_point(input, &point);
        const int dx = point.x - self->bridge_press_point_.x;
        const int dy = point.y - self->bridge_press_point_.y;
        if (dx > 12 || dx < -12 || dy > 12 || dy < -12) self->bridge_press_moved_ = true;
    }
    if (code != LV_EVENT_CLICKED) return;
    self->bridge_pressed_button_ = nullptr;
    // A slow sideways drag can emit CLICKED without a recognized gesture.
    // Remember total movement from press as well as LVGL's gesture state.
    if (self->bridge_press_moved_) return;
    if (input && lv_indev_get_gesture_dir(input) != LV_DIR_NONE) return;
    for (const auto &row : self->bridge_rows_) {
        if (row.button != target) continue;
        if (!vibe_pairing::select_bridge(row.id, row.url)) {
            lv_label_set_text(self->bridge_hint_, T("列表已更新，请重新选择"));
        } else {
            // Tasks and cached capabilities belong to the previous computer.
            // Fetch them again only after the chosen bridge is authenticated.
            self->invalidateCatalog(true);
            self->button_epoch_.fetch_add(1);
            std::lock_guard<std::mutex> lock(self->model_mutex_);
            self->model_.sessions.clear();
            self->model_.tasks.clear();
            self->model_.feedback.clear();
            self->model_.feedback_task_id.clear();
            self->model_.session_notice.clear();
            self->model_.action_in_flight = false;
            self->model_.connection_error = T("正在连接选中的电脑");
            self->pending_action_ = {};
            self->pending_create_session_ = false;
            self->visible_task_id_.clear();
            for (auto &provider : self->providers_) provider.selected_session_id.clear();
            ++self->model_.revision;
        }
        return;
    }
}

void VibeCoding::renderBridgeChoices(const vibe_pairing::Snapshot &pairing)
{
    if (!bridge_picker_) return;
    const bool choosing = pairing.access_mode == vibe_pairing::AccessMode::Automatic &&
                           pairing.phase == vibe_pairing::Phase::ChoosingBridge;
    if (!choosing || settings_open_.load()) {
        lv_obj_add_flag(bridge_picker_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (lv_obj_has_flag(bridge_picker_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(bridge_picker_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(bridge_picker_);
    }
    const bool wifi_online = vibe_wifi::is_connected();
    if (bridge_list_revision_ == pairing.revision && bridge_wifi_online_ == wifi_online) return;
    bridge_list_revision_ = pairing.revision;
    bridge_wifi_online_ = wifi_online;
    if (pairing.scanning || !wifi_online) lv_obj_add_state(bridge_refresh_button_, LV_STATE_DISABLED);
    else lv_obj_remove_state(bridge_refresh_button_, LV_STATE_DISABLED);
    lv_obj_clean(bridge_list_);
    bridge_rows_.clear();
    bridge_pressed_button_ = nullptr;
    lv_obj_scroll_to_y(bridge_list_, 0, LV_ANIM_OFF);
    for (const auto &bridge : pairing.bridges) {
        const int y = 8 + static_cast<int>(bridge_rows_.size()) * 64;
        lv_obj_t *button = touchButton(bridge_list_, 8, y, 254, 52,
                                       nullptr, bridgeSelectCallback, this);
        lv_obj_add_event_cb(button, bridgeSelectCallback, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(button, bridgeSelectCallback, LV_EVENT_PRESSING, this);
        lv_obj_t *name = settingsLabel(button, bridge.name.c_str(), 12, 6, 230, VT(title), false);
        lv_obj_set_height(name, made_s(20));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        const size_t scheme = bridge.url.find("://");
        const std::string address = bridge.url.substr(scheme == std::string::npos ? 0 : scheme + 3);
        lv_obj_t *host = settingsLabel(button, address.c_str(), 12, 28, 230, VT(text_secondary), false);
        lv_obj_set_style_text_font(host, &lv_font_montserrat_14, 0);
        lv_obj_set_height(host, made_s(16));
        lv_obj_set_style_text_align(host, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(host, LV_LABEL_LONG_DOT);
        lv_obj_remove_flag(host, LV_OBJ_FLAG_CLICKABLE);
        if (pairing.scanning || !wifi_online) lv_obj_add_state(button, LV_STATE_DISABLED);
        bridge_rows_.push_back({button, bridge.id, bridge.url});
    }
    lv_label_set_text(bridge_hint_, !wifi_online ? T("请先到桌面设置\n连接 Wi-Fi") : pairing.scanning ? T("正在扫描电脑…") :
        pairing.bridges.empty() ? T("没有找到电脑\n点刷新或去设置填地址") : T("点击电脑后核对配对码"));
}

void VibeCoding::noteComputerReply(const Task &task, const std::string &scope)
{
    const bool pending = task.status == "queued" || task.status == "running" ||
                         task.status == "waiting_confirmation";
    const bool arrived = (task.status == "completed" || task.status == "failed" ||
                          task.status == "handed_off") &&
                         (!task.result.empty() || !task.error.empty());
    if (scope != reply_watch_scope_) {
        // 换轨（切助手/切会话，含首帧）：旧会话的「在等回复」状态作废，
        // 只登记不响铃。否则上一任助手还没出结果的 pending 会撞上
        // 这一任助手的旧完成任务，凑出一次张冠李戴的「新消息」提示音。
        reply_watch_scope_ = scope;
        reply_watch_id_ = task.id;
        reply_saw_pending_ = pending;
        reply_wait_new_ = false;
        reply_watch_ready_ = true;
        return;
    }
    if (task.id != reply_watch_id_) {
        const bool expecting = reply_saw_pending_ || reply_wait_new_;
        reply_watch_id_ = task.id;
        reply_saw_pending_ = pending;
        reply_wait_new_ = false;
        if (arrived && expecting) {
            if (!drawer_open_.load()) unread_reply_ = true;
            made_chime_new_message();
        }
        return;
    }
    if (pending) reply_saw_pending_ = true;
    else if (arrived && reply_saw_pending_) {
        reply_saw_pending_ = false;
        reply_wait_new_ = false;
        if (!drawer_open_.load()) unread_reply_ = true;
        made_chime_new_message();
    }
}

void VibeCoding::updateStatusIndicators(bool provider_ready)
{
    if (!status_dot_) return;
    if (!provider_ready) {
        lv_obj_add_flag(status_dot_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(unread_mark_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    std::string connection_error;
    std::string task_status;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        connection_error = model_.connection_error;
        if (!model_.tasks.empty()) task_status = model_.tasks.front().status;
    }
    // 与精灵情绪同一套判据：离线 > 工作中 > 在线空闲。
    const bool working = task_status == "running" || task_status == "queued" ||
                         task_status == "waiting_confirmation";
    const bool offline = !connection_error.empty();
    lv_obj_remove_flag(status_dot_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(status_dot_, lv_color_hex(
        offline ? VT(danger) : working ? VT(warning) : VT(success)), 0);
    if (unread_reply_ && !offline) lv_obj_remove_flag(unread_mark_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(unread_mark_, LV_OBJ_FLAG_HIDDEN);
}

void VibeCoding::showAnswer(const std::string &scope, const std::string &text)
{
    if (!answer_area_ || !result_label_) return;
    const bool changed_task = answer_scope_ != scope;
    if (!changed_task && answer_text_ == text) return;
    const int32_t previous_scroll = changed_task ? 0 : lv_obj_get_scroll_y(answer_area_);
    ui_stage_.store(41);
    lv_label_set_text(result_label_, text.c_str());
    ui_stage_.store(42);
    answer_scope_ = scope;
    answer_text_ = text;
    lv_obj_update_layout(answer_area_);
    ui_stage_.store(43);
    // A regular 2.2-second task refresh must not send a reader who has
    // scrolled down a long answer back to the beginning.
    lv_obj_scroll_to_y(answer_area_, previous_scroll, LV_ANIM_OFF);
    ui_stage_.store(44);
}


void VibeCoding::render()
{
    ui_stage_.store(2);
    // 电源管理：空闲超时只关背光，任何触摸经由顶层遮罩唤醒（那一下触摸
    // 被遮罩吸收，不会触发底下的按钮）。心跳/连接等逻辑完全不受影响。
    if (screen_off_timeout_s_ > 0 && !screen_off_) {
        const uint32_t inactive = lv_display_get_inactive_time(NULL);
        if (inactive > screen_off_timeout_s_ * 1000U) applyScreenOff(true);
    }
    if (!instruction_label_) return;
    // 拖动看门狗：手指按住期间 PRESSING 事件每帧必到；断流超过 400ms 说明
    // 这次按压已经死亡（松手/丢失事件被丢，触摸驱动卡死等），home_dragging_
    // 若不强制清掉，归位逻辑全被跳过——整屏永久卡在偏移处，而后续触摸落在
    // 子控件上不会触发 screen 的 PRESSED 来复位。
    if (home_dragging_ && lv_tick_get() - last_pressing_ms_ > 400) {
        ESP_LOGW(BUTTON_TAG, "[ui] drag watchdog: pressing stale %ums, force release",
            static_cast<unsigned>(lv_tick_get() - last_pressing_ms_));
        home_dragging_ = false;
    }
    // 兑底：拖动被打断后若有残留的横向偏移，在下一帧强制归位。
    if (!home_dragging_) {
        lv_obj_t *scr = lv_scr_act();
        if (lv_obj_get_x(scr) != 0) lv_obj_set_x(scr, 0);
    }
    const auto voice = vibe_voice::status();
    // 精灵动效按回合驱动；必须放在 revision 提前返回之前，否则空闲时动画冻结。
    renderSpriteStage(voice);
    ui_stage_.store(3);
    const auto pairing = vibe_pairing::snapshot();
    ui_stage_.store(4);
    refreshLanguage();
    renderBridgeChoices(pairing);
    renderReceiverScan();
    renderWifiNetworks();
    renderThemes();
    renderFirmware();
    renderPhoneSetup();
    const bool catalog_ready = config_loaded_.load() &&
        pairing.phase == vibe_pairing::Phase::Paired && vibe_pairing::authorized_for_foreground();
    const int voice_phase = static_cast<int>(voice.phase);
    RenderViewCtx ctx;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (rendered_catalog_ready_ == catalog_ready && drawn_revision_ == model_.revision && last_pair_revision_ == pairing.revision &&
            voice_phase == last_voice_phase_ && voice.message == last_voice_message_) return;
        ctx.snapshot = model_;
        ctx.provider_index = selected_provider_.load();
        ctx.provider_count = providers_.size();
        ctx.provider = selectedProviderLocked();
        ctx.provider_ready = ctx.provider.available;
        ctx.selected_session_id = ctx.provider.selected_session_id;
    }
    ctx.pairing = pairing;
    ctx.voice = voice;
    ctx.catalog_ready = catalog_ready;
    ctx.own_error = vibe_i18n::message(ctx.snapshot.connection_error);
    ctx.own_feedback = ctx.snapshot.feedback_external ? vibe_i18n::transport_error(ctx.snapshot.feedback) : vibe_i18n::message(ctx.snapshot.feedback);
    ctx.own_voice_hint = vibe_i18n::message(ctx.snapshot.voice_hint);
    ctx.own_session_notice = vibe_i18n::message(ctx.snapshot.session_notice);
    ui_stage_.store(10);
    const int previous_voice_phase = last_voice_phase_;
    last_voice_phase_ = voice_phase;
    if (reply_watch_ready_ && previous_voice_phase != voice_phase &&
        (voice.phase == vibe_voice::Phase::Uploading || voice.phase == vibe_voice::Phase::Submitted)) {
        reply_wait_new_ = true;
    }
    last_voice_message_ = voice.message;
    last_pair_revision_ = pairing.revision;
    rendered_catalog_ready_ = catalog_ready;

    const bool ascii_label = std::all_of(ctx.provider.label.begin(), ctx.provider.label.end(),
        [](unsigned char c) { return c < 128; });
    lv_obj_set_style_text_font(provider_label_, ascii_label ?
        (made_fit() < made_design ? &lv_font_montserrat_16 : &lv_font_montserrat_26) : text_font, 0);
    lv_label_set_long_mode(provider_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(provider_label_, ctx.provider.label.c_str());
    const std::string page_number = std::to_string(ctx.provider_count ? ctx.provider_index + 1 : 0) + " / " + std::to_string(ctx.provider_count);
    lv_label_set_text(page_label_, page_number.c_str());
    if (ctx.catalog_ready && ctx.provider_count) {
        lv_obj_remove_flag(page_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(page_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(provider_label_, text_font, 0);
        lv_label_set_text(provider_label_, ctx.catalog_ready ? T("暂无助手") : T("连接电脑"));
    }
    // 顶部状态角标跟目录/连接状态每帧刷新（未配对时 catalog_ready 为假，自动隐藏）。
    updateStatusIndicators(ctx.catalog_ready && ctx.provider_count != 0);

    // 视图分发（视图函数见 vibe_app_render.cpp）。
    if (pairing.phase != vibe_pairing::Phase::Paired ||
        !vibe_pairing::authorized_for_foreground()) {
        renderPairingView(ctx);
        return;
    }
    if (pairing_drawer_auto_ && drawer_open_.load()) {
        pairing_drawer_auto_ = false;
        showDrawer(false);
    }
    if (ctx.catalog_ready && ctx.provider_count) lv_obj_remove_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
    if (showing_pairing_layout_) {
        lv_obj_add_flag(meta_label_, LV_OBJ_FLAG_HIDDEN);
        // 配对期间位置行可能被隐藏（避免与引导行重复），恢复会话头显示。
        lv_obj_remove_flag(position_label_, LV_OBJ_FLAG_HIDDEN);
        // 恢复抽屉版式：曾经这里恢复的是旧主页坐标 page_y(100)，回显区
        // 正好叠进指令行（page_y(104)），真机上两段文字叠印（2026-10-17 实锤）。
        applyDrawerAnswerLayout();
        lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_LEFT, 0);
        showing_pairing_layout_ = false;
    }
    if (!ctx.catalog_ready || !ctx.provider_count) {
        renderCatalogEmptyView(ctx);
        return;
    }
    ctx.voice_for_selected_session = !ctx.selected_session_id.empty() &&
        voice_session_id_ == ctx.selected_session_id;
    const bool show_voice_status = ctx.voice_for_selected_session &&
        (voice.phase == vibe_voice::Phase::Recording ||
         voice.phase == vibe_voice::Phase::Uploading ||
         (voice.phase == vibe_voice::Phase::Submitted && !submitted_task_seen_.load() && ctx.snapshot.tasks.empty()) ||
         voice.phase == vibe_voice::Phase::Error);
    ctx.voice_notice = show_voice_status && !voice.message.empty() ? voice.message :
        !ctx.snapshot.voice_hint.empty() ? ctx.own_voice_hint : "";

    const auto selected_session = std::find_if(ctx.snapshot.sessions.begin(), ctx.snapshot.sessions.end(),
        [&ctx](const Session &session) { return session.id == ctx.selected_session_id; });
    if (selected_session == ctx.snapshot.sessions.end()) lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
    if (ctx.selected_session_id != rendered_session_id_) {
        rendered_session_id_ = ctx.selected_session_id;
        displayed_task_id_.clear();
    }
    if (selected_session == ctx.snapshot.sessions.end()) {
        renderNoSessionView(ctx);
        return;
    }
    const std::string session_header = T("任务 ") +
        std::to_string(static_cast<int>(selected_session - ctx.snapshot.sessions.begin()) + 1) + "/" +
        std::to_string(ctx.snapshot.sessions.size());
    // 分页 chip 只承载序号，标题行只承载标题（详见抽屉重排注释）。
    lv_label_set_text(session_chip_label_, session_header.c_str());
    lv_obj_remove_flag(session_chip_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(position_label_, selected_session->title.empty() ? T("未命名任务") : selected_session->title.c_str());
    // 任务回显是长文，恢复左对齐（catalog/空会话状态曾切成居中）。
    lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_LEFT, 0);
    ui_stage_.store(20);

    if (ctx.voice_for_selected_session && voice.phase == vibe_voice::Phase::Submitted && !voice.task_id.empty() &&
        !submitted_task_seen_.load()) {
        const auto submitted = std::find_if(ctx.snapshot.tasks.begin(), ctx.snapshot.tasks.end(),
            [&voice](const Task &task) { return task.id == voice.task_id; });
        if (submitted == ctx.snapshot.tasks.end()) {
            // Do not leave an older actionable exchange on screen after the 201.
            displayed_task_id_.clear();
            renderStatusBadge("");
            setActivityRunning(false);
            lv_label_set_text(instruction_label_, T("新一轮已发送"));
            showAnswer(ctx.selected_session_id + "/submitted", T("正在读取刚发送的内容。三击 BOOT 可取消，双击请等回显。"));
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                visible_task_id_.clear();
            }
            renderAttachments({}); // 提交态无回显，清空可能残留的旧附件（放大浮层打开时安全跳过）
            layoutReadableText();
            drawn_revision_ = ctx.snapshot.revision;
            return;
        }
    }

    renderTaskView(ctx, selected_session == ctx.snapshot.sessions.end() ? nullptr : &*selected_session);
}

void VibeCoding::applyDrawerAnswerLayout()
{
    if (!answer_area_ || !result_label_) return;
    // 设计定稿：卡片左右贴 12 边距、顶吃掉指令行省出的一行（page_y 128）、
    // 底到 page_y(282)，距按钮行（page_y 296）留 14 设计像素不压。
    lv_obj_set_pos(answer_area_, made_x(12), made_page_y(128));
    lv_obj_set_size(answer_area_, made_s(336), made_page_y(282) - made_page_y(128));
    lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(result_label_, drawer_text_font, 0);
    // Air 语言：回显装进白卡片（面板底 + 细描边 + 大圆角，无投影），
    // 让「AI 说什么」有自己的容器层级，不再悬在页面上。
    lv_obj_set_style_bg_color(answer_area_, lv_color_hex(VT(bg_panel)), 0);
    lv_obj_set_style_bg_opa(answer_area_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(answer_area_, lv_color_hex(VT(border)), 0);
    lv_obj_set_style_border_width(answer_area_, 1, 0);
    lv_obj_set_style_radius(answer_area_, made_radius(16), 0);
    lv_obj_set_style_pad_all(answer_area_, made_s(12), 0);
    // 内容内缩交给 pad，result_label_ 归位到内容区原点；宽度必须显式钉在
    // 内容区（卡片宽 - 左右内边距），否则沿用主页旧宽会把字画出卡片右缘。
    lv_obj_set_pos(result_label_, 0, 0);
    lv_obj_set_width(result_label_, made_s(336) - made_s(24));
    lv_label_set_long_mode(result_label_, LV_LABEL_LONG_WRAP);
}

void VibeCoding::renderStatusBadge(const std::string &status)
{
    if (!status_badge_ || !status_badge_dot_ || !status_badge_label_) return;
    if (status.empty()) {
        lv_obj_add_flag(status_badge_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(status_badge_, LV_OBJ_FLAG_HIDDEN);
    // 默认中性（排队/已取消）；运行与完成走 accent 系，失败走 danger_soft。
    uint32_t bg = VT(bg_row), text = VT(text_secondary), dot = VT(text_hint);
    if (status == "running" || status == "completed" || status == "handed_off") {
        bg = VT(chip_accent);
        text = VT(text_accent);
        dot = VT(success);
    } else if (status == "failed") {
        bg = VT(danger_soft);
        text = VT(text_primary);
        dot = VT(danger);
    } else if (status == "waiting_confirmation") {
        dot = VT(warning);
    }
    lv_obj_set_style_bg_color(status_badge_, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(status_badge_dot_, lv_color_hex(dot), 0);
    // 右缘对齐：先落文字再量宽（徽章是 SIZE_CONTENT，先定位后改字会用上一
    // 个状态的旧宽——首帧宽度 0 直接把徽章定位到屏幕外，真机实锤），
    // 边距 12 设计像素，与卡片右缘（x348）里外呼应。
    lv_label_set_text(status_badge_label_, statusName(status));
    lv_obj_update_layout(status_badge_);
    lv_obj_set_pos(status_badge_, made_screen_w() - lv_obj_get_width(status_badge_) - made_s(12),
                   made_page_y(44));
    lv_obj_set_style_text_color(status_badge_label_, lv_color_hex(text), 0);
}

// —— 素材页顶部状态图标：单一位置，状态变了换图标 ——
// 状态优先级：录音 > 识别 > 提交等待建任务 > 等待确认 > 处理中 > 空闲。
// 位图由 tools/gen_status_icons.py 生成（16×16 白色+alpha），颜色走主题
// token recolor；抽屉/设置打开时隐藏（抽屉右上已有文字徽章）。
static void status_icon_spin_cb(void *var, int32_t v)
{
    lv_image_set_rotation(static_cast<lv_obj_t *>(var), v);
}

static void status_icon_pulse_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), static_cast<lv_opa_t>(v), 0);
}

void VibeCoding::updateStatusIcon(int voice_phase, const std::string &task_status, bool offline)
{
    (void)offline;   // 离线暂与空闲同图标；素材有 offline 情绪，图标不细分
    if (!status_icon_pill_ || !status_icon_img_) return;
    if (drawer_open_.load() || settings_open_.load()) {
        lv_obj_add_flag(status_icon_pill_, LV_OBJ_FLAG_HIDDEN);
        status_icon_state_ = -1;
        return;
    }
    int state = 0;
    // 优先级：用户正在说话 > 任务状态 > 语音提交残留。Submitted 在提交
    // 成功后不会自动复位（下次录音才覆盖），若让它压过任务状态，任务
    // 进入 waiting_confirmation 会显示处理中、完成后永远转圈（2026-10-20
    // 实测）。所以任务状态可见时一律以任务为准；Submitted 只兜底「任务
    // 还没出现在快照里」的窗口期，必须加 !submitted_task_seen_ 闸门——
    // 该任务被看到后 Submitted 一直残留，切到无任务的助手（快照空）时
    // 图标永远转圈（2026-10-20 晚实锤：心跳 fwTasks=0 fwTask=- 但图标在转）。
    if (voice_phase == static_cast<int>(vibe_voice::Phase::Recording)) state = 1;
    else if (voice_phase == static_cast<int>(vibe_voice::Phase::Uploading)) state = 2;
    else if (task_status == "waiting_confirmation") state = 3;
    else if (task_status == "running" || task_status == "queued") state = 4;
    else if (voice_phase == static_cast<int>(vibe_voice::Phase::Submitted) &&
             task_status.empty() && !submitted_task_seen_.load())
        state = 4;

    // 声波两帧交替（tick 驱动 160ms/帧），不占 anim 通道。
    if (state == 2 && status_icon_state_ == 2) {
        const uint32_t now = lv_tick_get();
        if (now - status_icon_wave_ms_ >= 160) {
            status_icon_wave_ms_ = now;
            status_icon_wave_alt_ = !status_icon_wave_alt_;
            lv_image_set_src(status_icon_img_,
                status_icon_wave_alt_ ? &kStatusIconWave2 : &kStatusIconWave1);
        }
    }

    if (state == status_icon_state_) return;
    status_icon_state_ = state;
    lv_anim_delete(status_icon_img_, nullptr);

    const lv_image_dsc_t *src = &kStatusIconIdle;
    uint32_t tint = VT(text_hint);
    switch (state) {
    case 1: src = &kStatusIconMic; tint = VT(danger); status_icon_wave_alt_ = false; break;
    case 2: src = &kStatusIconWave1; tint = VT(text_accent); status_icon_wave_alt_ = false; break;
    case 3: src = &kStatusIconAsk; tint = VT(warning); break;
    case 4: src = &kStatusIconRun; tint = VT(accent); break;
    default: break;
    }
    lv_image_set_src(status_icon_img_, src);
    lv_image_set_rotation(status_icon_img_, 0);
    lv_obj_set_style_opa(status_icon_img_, LV_OPA_COVER, 0);
    lv_obj_set_style_img_recolor(status_icon_img_, lv_color_hex(tint), 0);
    lv_obj_set_style_img_recolor_opa(status_icon_img_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(status_icon_pill_, LV_OBJ_FLAG_HIDDEN);
    if (state == 1) {
        // 录音中：呼吸脉冲（透明度 255↔140，800ms 往复）。
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, status_icon_img_);
        lv_anim_set_exec_cb(&a, status_icon_pulse_cb);
        lv_anim_set_values(&a, 255, 140);
        lv_anim_set_time(&a, 800);
        lv_anim_set_playback_time(&a, 800);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else if (state == 4) {
        // 处理中：缺口弧旋转（1s/圈，不确定式进度，不做假百分比）。
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, status_icon_img_);
        lv_anim_set_exec_cb(&a, status_icon_spin_cb);
        lv_anim_set_values(&a, 0, 3600);
        lv_anim_set_time(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

void VibeCoding::setActivityRunning(bool running)
{
    if (!activity_bar_ || !activity_fill_) return;
    if (!running) {
        lv_anim_delete(activity_fill_, nullptr);
        activity_anim_running_ = false;
        lv_obj_add_flag(activity_bar_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(activity_bar_, LV_OBJ_FLAG_HIDDEN);
    if (activity_anim_running_) return;
    const int32_t travel = made_s(312) - made_s(64);
    if (travel <= 0) return;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, activity_fill_);
    lv_anim_set_exec_cb(&anim, [](void *var, int32_t value) {
        lv_obj_set_x(static_cast<lv_obj_t *>(var), value);
    });
    lv_anim_set_values(&anim, 0, travel);
    lv_anim_set_duration(&anim, 900);
    lv_anim_set_playback_duration(&anim, 900);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
    activity_anim_running_ = true;
}

void VibeCoding::layoutReadableText()
{
    if (!made_rect() || made_fit() >= made_design || !provider_label_ || !position_label_ ||
        !instruction_label_ || !answer_area_ || !result_label_) return;
    // 这些标签已搬进抽屉时按抽屉版式显示，不再套主页坐标。
    if (drawer_ && lv_obj_get_parent(position_label_) == drawer_) return;
    const int margin = 8;
    const int width = made_screen_w() - margin * 2;
    const int line = lv_font_get_line_height(text_font);
    int y = made_y(28) + made_s(74) + 8;
    auto row = [&](lv_obj_t *label) {
        if (!label || lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) return;
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, width);
        lv_obj_set_pos(label, margin, y);
        lv_obj_set_style_transform_scale_x(label, 256, 0);
        lv_obj_set_style_transform_scale_y(label, 256, 0);
        lv_obj_update_layout(label);
        const int height = lv_obj_get_height(label);
        y += (height > 0 ? height : line) + 8;
    };
    row(provider_label_);
    const int title_y = y;
    row(position_label_);
    if (session_title_tap_) {
        lv_obj_set_pos(session_title_tap_, margin, title_y);
        lv_obj_set_size(session_title_tap_, width, y - title_y);
    }
    row(instruction_label_);
    row(meta_label_);
    const int bottom = made_screen_h() - 8;
    int answer_h = bottom - y;
    if (answer_h < line * 3) answer_h = line * 3;
    lv_obj_set_pos(answer_area_, margin, y);
    lv_obj_set_size(answer_area_, width, answer_h);
    lv_obj_set_style_pad_all(answer_area_, 0, 0);
    lv_obj_set_overflow_visible(answer_area_, false);
    lv_obj_set_pos(result_label_, 0, 0);
    lv_label_set_long_mode(result_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(result_label_, width - 4);
    lv_obj_set_style_transform_scale_x(result_label_, 256, 0);
    lv_obj_set_style_transform_scale_y(result_label_, 256, 0);
}

void VibeCoding::setConnectionError(std::string error)
{
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (model_.connection_error != error) {
        model_.connection_error = std::move(error);
        ++model_.revision;
    }
}


void VibeCoding::setVolumeLevel(int level)
{
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    if (bsp_extra_out_volume_set(level) != ESP_OK) return;
    renderVolume();
}

void VibeCoding::selectProviderById(const std::string &id)
{
    const auto voice = vibe_voice::status();
    if (voice.phase == vibe_voice::Phase::Recording || voice.phase == vibe_voice::Phase::Uploading) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.voice_hint = "Finish voice capture before switching agent";
        ++model_.revision;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (!config_loaded_.load() || providers_.empty()) return;
        int index = -1;
        for (int i = 0; i < static_cast<int>(providers_.size()); ++i) {
            if (providers_[i].id == id) index = i;
        }
        if (index < 0) {
            model_.session_notice = T("没有这个助手");
            ++model_.revision;
            return;
        }
        if (index == selected_provider_.load()) return;
        submitted_task_seen_ = true;
        displayed_task_id_.clear();
        rendered_session_id_.clear();
        selected_provider_.store(index);
        button_epoch_.fetch_add(1);
        model_.sessions.clear();
        model_.tasks.clear();
        model_.feedback.clear();
        model_.feedback_task_id.clear();
        model_.voice_hint.clear();
        model_.session_notice.clear();
        model_.connection_error = "Loading sessions...";
        visible_task_id_.clear();
        ++model_.revision;
    }
    drawn_revision_ = UINT32_MAX;
}

void VibeCoding::selectSessionById(const std::string &id)
{
    const auto voice = vibe_voice::status();
    if (voice.phase == vibe_voice::Phase::Recording || voice.phase == vibe_voice::Phase::Uploading) return;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (!config_loaded_.load()) return;
        const auto found = std::find_if(model_.sessions.begin(), model_.sessions.end(), [&id](const Session &session) {
            return session.id == id;
        });
        auto *provider = findProviderLocked(selectedProviderLocked().id);
        if (!provider || found == model_.sessions.end()) {
            model_.session_notice = T("没有这个任务");
            ++model_.revision;
            return;
        }
        if (provider->selected_session_id == id) return;
        provider->selected_session_id = id;
        model_.tasks.clear();
        model_.feedback.clear();
        model_.feedback_task_id.clear();
        model_.session_notice.clear();
        visible_task_id_.clear();
        ++model_.revision;
    }
    button_epoch_.fetch_add(1);
    submitted_task_seen_ = true;
    displayed_task_id_.clear();
    rendered_session_id_.clear();
    drawn_revision_ = UINT32_MAX;
}

void VibeCoding::queueDeleteSessionId(const std::string &id)
{
    const auto voice = vibe_voice::status();
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (voice.phase == vibe_voice::Phase::Recording || voice.phase == vibe_voice::Phase::Uploading) {
        model_.session_notice = T("录音中，请稍后删除任务");
        ++model_.revision;
        return;
    }
    const auto &provider = selectedProviderLocked();
    const auto found = std::find_if(model_.sessions.begin(), model_.sessions.end(), [&id](const Session &session) {
        return session.id == id;
    });
    if (found == model_.sessions.end() || pending_delete_session_ || delete_session_in_flight_) {
        if (found == model_.sessions.end()) {
            model_.session_notice = T("没有这个任务");
            ++model_.revision;
        }
        return;
    }
    pending_delete_provider_id_ = provider.id;
    pending_delete_session_id_ = id;
    pending_delete_session_ = true;
    model_.session_notice = T("正在删除任务…");
    ++model_.revision;
}

void VibeCoding::startThemeApply(const std::string &name)
{
    const auto pairing = vibe_pairing::snapshot();
    const bool usb_mode = pairing.access_mode == vibe_pairing::AccessMode::UsbDirect;
    std::string base = !pairing.url.empty() ? pairing.url : pairing.manual_url;
    if (!usb_mode && base.empty()) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.session_notice = T("尚未连接电脑，无法应用");
        ++model_.revision;
        return;
    }
    struct ApplyArgs {
        VibeCoding *self;
        std::string name;
        std::string base;
        bool usb_mode;
    };
    const std::string theme_name = name == "default" ? std::string() : name;
    if (xTaskCreate([](void *arg) {
            auto *args = static_cast<ApplyArgs *>(arg);
            const esp_err_t result = args->usb_mode
                                         ? vibe_theme::applyFromBridge(args->name, "")
                                         : vibe_theme::applyFromBridge(args->name, args->base);
            if (result == ESP_OK) args->self->theme_reboot_pending_.store(true);
            delete args;
            vTaskDelete(nullptr);
        }, "vibe_theme_cmd", 12288, new ApplyArgs{this, theme_name, base, usb_mode}, 3, nullptr) != pdPASS) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.session_notice = T("应用任务启动失败");
        ++model_.revision;
    }
}



} // namespace esp_brookesia::apps
