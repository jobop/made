// SPDX-License-Identifier: Apache-2.0
#include "esp_brookesia_app_vibe.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

#include "boot_click_gesture.hpp"
#include "made_lock_screen.hpp"
#include "made_layout.hpp"
#include "new_message_chime.hpp"
#include "cJSON.h"
#include "bsp/display.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "vibe_pairing.hpp"
#include "vibe_voice_capture.hpp"
#include "vibe_wifi.hpp"
#include "vibe_usb.hpp"
#include "vibe_i18n.hpp"
#include "vibe_touch_keyboard.hpp"
#include "vibe_phone_setup.hpp"

using esp_brookesia::systems::phone::App;

LV_IMG_DECLARE(img_app_vibe);
LV_FONT_DECLARE(font_puhui_16_4);

namespace {
using vibe_i18n::tr;
const char *T(const char *key) { return tr(key); }
struct LocalizedLabel { lv_obj_t *object; std::string key; };
std::vector<LocalizedLabel> localized_labels; // LVGL thread only; all belong to this app screen.
void bindLabel(lv_obj_t *label, const char *key) {
    if (key && vibe_i18n::known(key)) localized_labels.push_back({label, key});
}

constexpr size_t MAX_RESPONSE_BYTES = 32 * 1024;
constexpr uint32_t POLL_INTERVAL_MS = 2200;
constexpr uint64_t HEARTBEAT_INTERVAL_MS = 12000;
constexpr char BUTTON_TAG[] = "vibe_boot";

constexpr App::Config vibePhoneConfig()
{
    auto config = App::Config::SIMPLE_CONSTRUCTOR(&img_app_vibe, false, false);
    // Vibe owns horizontal assistant swipes and BOOT owns exit. The system's
    // edge gesture mask must not intercept either its buttons or answer area.
    config.flags.enable_navigation_gesture = false;
    return config;
}

bool receiverVoiceUploading()
{
    const auto mode = vibe_pairing::snapshot().access_mode;
    return (mode == vibe_pairing::AccessMode::Receiver || mode == vibe_pairing::AccessMode::UsbDirect) &&
           vibe_voice::status().phase == vibe_voice::Phase::Uploading;
}

// The bundled Puhui font includes common Han characters missing from LVGL's
// small Source Han Sans subset. Its glyph data lives in flash, not PSRAM.
const lv_font_t *const text_font = &font_puhui_16_4;

constexpr uintptr_t kPageSurface = 0x50414745;

void markPage(lv_obj_t *object)
{
    if (object) lv_obj_set_user_data(object, reinterpret_cast<void *>(kPageSurface));
}

bool isPage(lv_obj_t *object)
{
    return object && lv_obj_get_user_data(object) == reinterpret_cast<void *>(kPageSurface);
}

// Side buttons stay tappable; the title uses the gap between them so 16 px
// text is not drawn across the buttons on a narrower panel.
void placePageHeader(lv_obj_t *left, lv_obj_t *title, lv_obj_t *right)
{
    if (!made_rect()) return;
    const int margin = 8;
    const int y = 8;
    const int height = 40;
    const int width = 72;
    if (left) {
        lv_obj_set_pos(left, margin, y);
        lv_obj_set_size(left, width, height);
    }
    if (right) {
        lv_obj_set_pos(right, made_screen_w() - margin - width, y);
        lv_obj_set_size(right, width, height);
    }
    if (!title) return;
    const int left_edge = left ? margin + width + 6 : margin;
    const int right_edge = right ? made_screen_w() - margin - width - 6 : made_screen_w() - margin;
    int box = right_edge - left_edge;
    if (box < 1) box = 1;
    lv_obj_set_pos(title, left_edge, y + 11);
    lv_obj_set_width(title, box);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
}

lv_obj_t *touchButton(lv_obj_t *parent, int x, int y, int width, int height,
                      const char *caption, lv_event_cb_t callback, void *user,
                      uint32_t color = 0x284668)
{
    const bool page = isPage(parent);
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, page ? made_page_x(x) : made_x(x), page ? made_page_y(y) : made_y(y));
    lv_obj_set_size(button, page ? made_page_w(width) : made_s(width), page ? made_page_h(height) : made_s(height));
    lv_obj_set_style_radius(button, made_s(12) > 0 ? made_s(12) : 1, 0);
    lv_obj_set_overflow_visible(button, true);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x83B5E6), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user);
    if (caption) {
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, T(caption));
        bindLabel(label, caption);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xF1F5FF), 0);
        lv_obj_set_style_text_font(label, text_font, 0);
        made_text(label, width - 12, true);
        lv_obj_center(label);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return button;
}

lv_obj_t *settingsLabel(lv_obj_t *parent, const char *caption, int x, int y, int width,
                        uint32_t color = 0xE7EEFF, bool localize = true)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, localize ? T(caption) : caption);
    if (localize) bindLabel(label, caption);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, text_font, 0);
    if (isPage(parent)) made_page_label(label, x, y, width);
    else {
        lv_obj_set_pos(label, made_x(x), made_y(y));
        made_text(label, width, false);
    }
    return label;
}

std::string jsonString(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

bool validTaskId(const std::string &id)
{
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

bool validProjectId(const std::string &id)
{
    if (id.empty() || id.size() > 40) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool validSessionId(const std::string &id)
{
    if (id.size() != 36) return false;
    for (size_t index = 0; index < id.size(); ++index) {
        const char c = id[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

const char *statusName(const std::string &status)
{
    if (status == "waiting_confirmation") return T("待确认");
    if (status == "queued") return T("已排队");
    if (status == "running") return T("运行中");
    if (status == "completed") return T("已完成");
    if (status == "failed") return T("失败");
    if (status == "cancelled") return T("已取消");
    if (status == "handed_off") return T("已转交");
    return T("未知状态");
}

} // namespace

namespace esp_brookesia::apps {

VibeCoding *VibeCoding::_instance = nullptr;

VibeCoding *VibeCoding::requestInstance()
{
    if (!_instance) _instance = new VibeCoding();
    return _instance;
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
    enterLockScreen();
    invalidateCatalog(true);
    button_epoch_.fetch_add(1);
    active_ = true;
    exit_requested_ = false;
    rendered_session_id_.clear();
    last_provider_swipe_ms_ = 0;
    last_session_swipe_ms_ = 0;
    drawn_revision_ = UINT32_MAX;
    showing_pairing_layout_ = false;

    localized_labels.clear();
    last_locale_revision_ = UINT32_MAX;
    lv_obj_t *screen = lv_scr_act();
    // Brookesia's default screen is not clickable. Restore hit testing so
    // swipes beginning on blank space or a non-clickable label reach Vibe.
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x091321), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(screen, providerSwipeCallback, LV_EVENT_GESTURE, this);
    // Vertical swipes on the header browse sessions. The answer area below owns
    // its own vertical scrolling and does not bubble gestures to the screen.
    lv_obj_add_event_cb(screen, sessionSwipeCallback, LV_EVENT_GESTURE, this);

    // Artwork arrives with the authorized bridge catalog. Only the selected
    // image is expanded; the other entries retain their bounded packed pixels.
    animal_halo_ = lv_obj_create(screen);
    lv_obj_set_pos(animal_halo_, made_x(143), made_y(28));
    lv_obj_set_size(animal_halo_, made_s(74), made_s(74));
    lv_obj_set_style_bg_color(animal_halo_, lv_color_hex(0x263B59), 0);
    lv_obj_set_style_border_width(animal_halo_, 2, 0);
    lv_obj_set_style_radius(animal_halo_, 37, 0);
    lv_obj_set_style_pad_all(animal_halo_, 0, 0);
    lv_obj_clear_flag(animal_halo_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(animal_halo_, LV_OBJ_FLAG_SCROLLABLE);
    provider_image_ = lv_image_create(animal_halo_);
    lv_obj_clear_flag(provider_image_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(provider_image_, LV_OBJ_FLAG_HIDDEN);
    icon_placeholder_ = lv_label_create(animal_halo_);
    lv_label_set_text(icon_placeholder_, "?");
    lv_obj_set_style_text_font(icon_placeholder_, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(icon_placeholder_, lv_color_hex(0x9FADD0), 0);
    lv_obj_center(icon_placeholder_);
    lv_obj_add_flag(animal_halo_, LV_OBJ_FLAG_HIDDEN);
    rendered_icon_ = {};
    rendered_catalog_ready_ = false;

    page_label_ = lv_label_create(screen);
    lv_obj_set_pos(page_label_, made_x(246), made_y(111));
    lv_obj_set_width(page_label_, made_s(60));
    lv_obj_set_style_text_align(page_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(page_label_, lv_color_hex(0x9FADD0), 0);
    lv_obj_set_style_text_font(page_label_, &lv_font_montserrat_12, 0);

    // Creating a conversation is explicit. This touch target is large enough
    // for the small round screen and never doubles as the record button.
    new_session_button_ = lv_obj_create(screen);
    lv_obj_set_pos(new_session_button_, made_x(270), made_y(59));
    lv_obj_set_size(new_session_button_, made_s(40), made_s(40));
    lv_obj_set_style_radius(new_session_button_, 20, 0);
    lv_obj_set_style_bg_color(new_session_button_, lv_color_hex(0x284668), 0);
    lv_obj_set_style_border_color(new_session_button_, lv_color_hex(0x83B5E6), 0);
    lv_obj_set_style_border_width(new_session_button_, 1, 0);
    lv_obj_set_style_pad_all(new_session_button_, 0, 0);
    lv_obj_remove_flag(new_session_button_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(new_session_button_, newSessionCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *new_session_mark = lv_label_create(new_session_button_);
    lv_label_set_text(new_session_mark, "+");
    lv_obj_set_style_text_color(new_session_mark, lv_color_hex(0xF1F5FF), 0);
    lv_obj_set_style_text_font(new_session_mark, &lv_font_montserrat_26, 0);
    lv_obj_remove_flag(new_session_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(new_session_mark);

    delete_session_button_ = lv_obj_create(screen);
    lv_obj_set_pos(delete_session_button_, made_x(225), made_y(59));
    lv_obj_set_size(delete_session_button_, made_s(40), made_s(40));
    lv_obj_set_style_radius(delete_session_button_, 20, 0);
    lv_obj_set_style_bg_color(delete_session_button_, lv_color_hex(0x284668), 0);
    lv_obj_set_style_border_color(delete_session_button_, lv_color_hex(0x83B5E6), 0);
    lv_obj_set_style_border_width(delete_session_button_, 1, 0);
    lv_obj_set_style_pad_all(delete_session_button_, 0, 0);
    lv_obj_remove_flag(delete_session_button_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(delete_session_button_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(delete_session_button_, deleteSessionCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *delete_session_mark = lv_label_create(delete_session_button_);
    lv_label_set_text(delete_session_mark, "-");
    lv_obj_set_style_text_color(delete_session_mark, lv_color_hex(0xF1F5FF), 0);
    lv_obj_set_style_text_font(delete_session_mark, &lv_font_montserrat_26, 0);
    lv_obj_remove_flag(delete_session_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(delete_session_mark);
    lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);

    // The access editor is a modal surface. Its touch keyboard fits within
    // the round screen, and BOOT short clicks are ignored while it is open.
    settings_button_ = touchButton(screen, 50, 59, 40, 40, nullptr, settingsCallback, this);
    lv_obj_set_style_radius(settings_button_, 20, 0);
    constexpr int teeth[][2] = {{16, 5}, {16, 27}, {5, 16}, {27, 16},
                                {9, 9}, {25, 9}, {9, 25}, {25, 25}};
    for (const auto &tooth : teeth) {
        lv_obj_t *part = lv_obj_create(settings_button_);
        lv_obj_set_pos(part, made_s(tooth[0]), made_s(tooth[1]));
        lv_obj_set_size(part, made_s(8), made_s(8));
        lv_obj_set_style_bg_color(part, lv_color_hex(0xF1F5FF), 0);
        lv_obj_set_style_border_width(part, 0, 0);
        lv_obj_set_style_pad_all(part, 0, 0);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t *gear_ring = lv_obj_create(settings_button_);
    lv_obj_set_pos(gear_ring, made_s(10), made_s(10));
    lv_obj_set_size(gear_ring, made_s(20), made_s(20));
    lv_obj_set_style_radius(gear_ring, 10, 0);
    lv_obj_set_style_bg_color(gear_ring, lv_color_hex(0xF1F5FF), 0);
    lv_obj_set_style_border_width(gear_ring, 0, 0);
    lv_obj_set_style_pad_all(gear_ring, 0, 0);
    lv_obj_remove_flag(gear_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *gear_hole = lv_obj_create(gear_ring);
    lv_obj_set_pos(gear_hole, made_s(6), made_s(6));
    lv_obj_set_size(gear_hole, made_s(8), made_s(8));
    lv_obj_set_style_radius(gear_hole, 4, 0);
    lv_obj_set_style_bg_color(gear_hole, lv_color_hex(0x284668), 0);
    lv_obj_set_style_border_width(gear_hole, 0, 0);
    lv_obj_remove_flag(gear_hole, LV_OBJ_FLAG_CLICKABLE);

    settings_overlay_ = lv_obj_create(screen);
    lv_obj_set_pos(settings_overlay_, made_s(0), made_s(0));
    lv_obj_set_size(settings_overlay_, made_screen_w(), made_screen_h());
    lv_obj_set_style_bg_color(settings_overlay_, lv_color_hex(0x091321), 0);
    lv_obj_set_style_bg_opa(settings_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_overlay_, 0, 0);
    lv_obj_set_style_pad_all(settings_overlay_, 0, 0);
    lv_obj_remove_flag(settings_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(settings_overlay_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    settings_home_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(settings_home_, made_screen_w(), made_screen_h());
    markPage(settings_home_);
    lv_obj_set_style_bg_opa(settings_home_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_home_, 0, 0);
    lv_obj_set_style_pad_all(settings_home_, 0, 0);
    lv_obj_remove_flag(settings_home_, LV_OBJ_FLAG_SCROLLABLE);
    settingsLabel(settings_home_, T("设置"), 92, 48, 176);
    touchButton(settings_home_, 63, 100, 234, 52, "settings.connection", accessOpenCallback, this);
    touchButton(settings_home_, 63, 160, 234, 52, T("电源设置"), powerOpenCallback, this);
    touchButton(settings_home_, 63, 220, 234, 52, "settings.language", languageOpenCallback, this);
    touchButton(settings_home_, 125, 279, 110, 38, T("返回"), settingsCloseCallback, this);

    language_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(language_panel_, made_screen_w(), made_screen_h());
    markPage(language_panel_);
    lv_obj_set_style_bg_opa(language_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(language_panel_, 0, 0);
    lv_obj_set_style_pad_all(language_panel_, 0, 0);
    lv_obj_remove_flag(language_panel_, LV_OBJ_FLAG_SCROLLABLE);
    settingsLabel(language_panel_, "settings.language", 70, 38, 220);
    language_zh_button_ = touchButton(language_panel_, 70, 85, 220, 46, "中文", languageCallback, this);
    language_en_button_ = touchButton(language_panel_, 70, 143, 220, 46, "English", languageCallback, this);
    language_auto_button_ = touchButton(language_panel_, 70, 201, 220, 46, T("跟随电脑"), languageCallback, this);
    language_hint_ = settingsLabel(language_panel_, T("设备语言独立保存"), 55, 259, 250, 0x9FADD0);
    touchButton(language_panel_, 125, 292, 110, 34, T("返回"), languageBackCallback, this);
    lv_obj_add_flag(language_panel_, LV_OBJ_FLAG_HIDDEN);

    power_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(power_panel_, made_screen_w(), made_screen_h());
    markPage(power_panel_);
    lv_obj_set_style_bg_opa(power_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(power_panel_, 0, 0);
    lv_obj_set_style_pad_all(power_panel_, 0, 0);
    lv_obj_remove_flag(power_panel_, LV_OBJ_FLAG_SCROLLABLE);
    settingsLabel(power_panel_, T("电源设置"), 70, 38, 220);
    settingsLabel(power_panel_, T("空闲自动熄屏"), 55, 100, 250, 0xB8C9E4);
    power_timeout_button_ = touchButton(power_panel_, 70, 130, 220, 46, nullptr, powerTimeoutCallback, this);
    power_timeout_label_ = settingsLabel(power_timeout_button_, "", 0, 12, 220);
    settingsLabel(power_panel_, T("熄屏只关背光，连接与心跳不受影响；触摸屏幕即可点亮"), 45, 205, 270, 0x9FADD0);
    touchButton(power_panel_, 125, 292, 110, 34, T("返回"), powerBackCallback, this);
    lv_obj_add_flag(power_panel_, LV_OBJ_FLAG_HIDDEN);
    loadScreenOffTimeout();

    access_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(access_panel_, made_screen_w(), made_screen_h());
    markPage(access_panel_);
    lv_obj_set_style_bg_opa(access_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(access_panel_, 0, 0);
    lv_obj_set_style_pad_all(access_panel_, 0, 0);
    lv_obj_remove_flag(access_panel_, LV_OBJ_FLAG_SCROLLABLE);
    // Keep the editor actions near the top of the round display. Buttons at
    // y=314 were inside the drawn circle but unreliable at its touch edge.
    lv_obj_t *access_back = touchButton(access_panel_, 86, 29, 58, 36, T("返回"), accessBackCallback, this, 0x42566B);
    lv_obj_t *access_title = settingsLabel(access_panel_, T("接入设置"), 144, 39, 72);
    access_save_button_ = touchButton(access_panel_, 216, 29, 58, 36, T("保存"), accessSaveCallback, this, 0x1C856F);
    placePageHeader(access_back, access_title, access_save_button_);
    // Choose a transport separately from editing its fields. Four narrow tabs
    // were easy to mistap on this round display; these targets have 18 px gaps.
    access_picker_hint_ = settingsLabel(access_panel_, T("选择连接方式"), 65, 78, 230, 0xB8C9E4);
    access_auto_button_ = touchButton(access_panel_, 45, 104, 126, 64,
                                      "Wi-Fi", accessModeCallback, this);
    access_manual_button_ = touchButton(access_panel_, 189, 104, 126, 64,
                                        T("手动地址"), accessModeCallback, this);
    access_receiver_button_ = touchButton(access_panel_, 45, 186, 126, 64,
                                          T("接收端"), accessModeCallback, this);
    access_usb_button_ = touchButton(access_panel_, 189, 186, 126, 64,
                                     T("USB 直连"), accessModeCallback, this);
    phone_setup_button_ = touchButton(access_panel_, 70, 273, 220, 44,
                                       "手机配置", phoneSetupOpenCallback, this, 0x1C856F);
    access_change_button_ = touchButton(access_panel_, 54, 74, 252, 32,
                                        nullptr, accessPickerCallback, this);
    access_change_label_ = settingsLabel(access_change_button_, "", 0, 6, 252);
    lv_obj_set_pos(access_change_label_, made_s(0), made_s(6));
    lv_obj_remove_flag(access_change_label_, LV_OBJ_FLAG_CLICKABLE);
    access_status_label_ = settingsLabel(access_panel_, T("公网地址请选择 HTTPS"), 47, 109, 266, 0x8CE4CB);
    lv_label_set_long_mode(access_status_label_, LV_LABEL_LONG_DOT);
    access_usb_hint_ = settingsLabel(access_panel_,
        T("用数据线连接电脑\n打开电脑桥接器\n保存后核对六位配对码"), 65, 152, 230, 0xB8C9E4);
    lv_obj_set_style_text_line_space(access_usb_hint_, 10, 0);
    lv_obj_add_flag(access_usb_hint_, LV_OBJ_FLAG_HIDDEN);
    access_host_input_ = lv_textarea_create(access_panel_);
    lv_obj_set_pos(access_host_input_, made_page_x(54), made_page_y(131));
    lv_obj_set_size(access_host_input_, made_page_w(252), made_page_h(31));
    lv_textarea_set_one_line(access_host_input_, true);
    lv_textarea_set_max_length(access_host_input_, 100);
    lv_textarea_set_placeholder_text(access_host_input_, T("主机名或 IPv4 地址"));
    lv_obj_set_style_text_font(access_host_input_, text_font, LV_PART_MAIN);
    lv_obj_set_style_text_font(access_host_input_, text_font, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(access_host_input_, accessFieldCallback, LV_EVENT_CLICKED, this);
    access_port_input_ = lv_textarea_create(access_panel_);
    lv_obj_set_pos(access_port_input_, made_page_x(54), made_page_y(165));
    lv_obj_set_size(access_port_input_, made_page_w(80), made_page_h(30));
    lv_textarea_set_one_line(access_port_input_, true);
    lv_textarea_set_max_length(access_port_input_, 5);
    lv_textarea_set_accepted_chars(access_port_input_, "0123456789");
    lv_textarea_set_placeholder_text(access_port_input_, T("端口"));
    lv_obj_set_style_text_font(access_port_input_, text_font, LV_PART_MAIN);
    lv_obj_set_style_text_font(access_port_input_, text_font, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(access_port_input_, accessFieldCallback, LV_EVENT_CLICKED, this);
    access_password_input_ = lv_textarea_create(access_panel_);
    lv_obj_set_pos(access_password_input_, made_page_x(54), made_page_y(165));
    lv_obj_set_size(access_password_input_, made_page_w(252), made_page_h(30));
    lv_textarea_set_one_line(access_password_input_, true);
    lv_textarea_set_max_length(access_password_input_, 63);
    lv_textarea_set_password_mode(access_password_input_, true);
    lv_textarea_set_placeholder_text(access_password_input_, T("密码（留空沿用已保存的）"));
    lv_obj_set_style_text_font(access_password_input_, text_font, LV_PART_MAIN);
    lv_obj_set_style_text_font(access_password_input_, text_font, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(access_password_input_, accessFieldCallback, LV_EVENT_CLICKED, this);
    // Wi-Fi 选网的密码承载框：仅作为编辑器的写回目标，保持隐藏。
    wifi_password_input_ = lv_textarea_create(access_panel_);
    lv_textarea_set_one_line(wifi_password_input_, true);
    lv_textarea_set_max_length(wifi_password_input_, 63);
    lv_textarea_set_password_mode(wifi_password_input_, true);
    lv_obj_add_flag(wifi_password_input_, LV_OBJ_FLAG_HIDDEN);
    access_https_button_ = touchButton(access_panel_, 149, 165, 158, 30,
                                       nullptr, accessHttpsCallback, this);
    access_https_label_ = settingsLabel(access_https_button_, T("HTTPS · 开"), 0, 6, 158);
    lv_obj_set_pos(access_https_label_, made_s(0), made_s(6));
    lv_obj_remove_flag(access_https_label_, LV_OBJ_FLAG_CLICKABLE);
    receiver_scan_button_ = touchButton(access_panel_, 65, 215, 230, 46,
        "选择接收端", receiverScanOpenCallback, this);
    lv_obj_add_flag(receiver_scan_button_, LV_OBJ_FLAG_HIDDEN);
    receiver_picker_ = lv_obj_create(access_panel_);
    lv_obj_set_pos(receiver_picker_, made_s(0), made_s(0));
    lv_obj_set_size(receiver_picker_, made_screen_w(), made_screen_h());
    markPage(receiver_picker_);
    lv_obj_set_style_bg_color(receiver_picker_, lv_color_hex(0x091321), 0);
    lv_obj_set_style_bg_opa(receiver_picker_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(receiver_picker_, 0, 0);
    lv_obj_set_style_pad_all(receiver_picker_, 0, 0);
    lv_obj_remove_flag(receiver_picker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(receiver_picker_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    touchButton(receiver_picker_, 78, 29, 76, 38, "返回", receiverScanBackCallback, this);
    receiver_refresh_button_ = touchButton(receiver_picker_, 206, 29, 76, 38,
        "刷新", receiverScanRefreshCallback, this);
    settingsLabel(receiver_picker_, "选择接收端", 65, 79, 230);
    receiver_list_ = lv_obj_create(receiver_picker_);
    lv_obj_set_pos(receiver_list_, made_page_x(45), made_page_y(110));
    lv_obj_set_size(receiver_list_, made_page_w(270), made_page_y(268) - made_page_y(110));
    lv_obj_set_style_bg_opa(receiver_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(receiver_list_, 0, 0);
    lv_obj_set_style_pad_all(receiver_list_, 0, 0);
    lv_obj_set_scroll_dir(receiver_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(receiver_list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(receiver_list_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    receiver_hint_ = settingsLabel(receiver_picker_, "", 60, 274, 240, 0xB8C9E4);
    touchButton(receiver_picker_, 125, 304, 110, 34, "手动填写", receiverScanBackCallback, this);
    lv_obj_add_flag(receiver_picker_, LV_OBJ_FLAG_HIDDEN);
    receiver_scan_revision_ = UINT32_MAX;
    // Wi-Fi 模式页里的扫描入口与选网卡子页（布局沿用接收端选择器）。
    wifi_scan_button_ = touchButton(access_panel_, 65, 215, 230, 46,
        T("扫描 Wi-Fi 网络"), wifiScanOpenCallback, this, 0x1C856F);
    lv_obj_add_flag(wifi_scan_button_, LV_OBJ_FLAG_HIDDEN);
    wifi_picker_ = lv_obj_create(access_panel_);
    lv_obj_set_pos(wifi_picker_, made_s(0), made_s(0));
    lv_obj_set_size(wifi_picker_, made_screen_w(), made_screen_h());
    markPage(wifi_picker_);
    lv_obj_set_style_bg_color(wifi_picker_, lv_color_hex(0x091321), 0);
    lv_obj_set_style_bg_opa(wifi_picker_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_picker_, 0, 0);
    lv_obj_set_style_pad_all(wifi_picker_, 0, 0);
    lv_obj_remove_flag(wifi_picker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wifi_picker_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    touchButton(wifi_picker_, 78, 29, 76, 38, "返回", wifiScanBackCallback, this);
    touchButton(wifi_picker_, 206, 29, 76, 38, "刷新", wifiScanRefreshCallback, this);
    settingsLabel(wifi_picker_, "选择 Wi-Fi 网络", 65, 79, 230);
    wifi_network_list_ = lv_obj_create(wifi_picker_);
    lv_obj_set_pos(wifi_network_list_, made_page_x(45), made_page_y(110));
    lv_obj_set_size(wifi_network_list_, made_page_w(270), made_page_y(268) - made_page_y(110));
    lv_obj_set_style_bg_opa(wifi_network_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_network_list_, 0, 0);
    lv_obj_set_style_pad_all(wifi_network_list_, 0, 0);
    lv_obj_set_scroll_dir(wifi_network_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wifi_network_list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(wifi_network_list_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    wifi_scan_status_ = settingsLabel(wifi_picker_, "", 60, 274, 240, 0xB8C9E4);
    lv_obj_add_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN);
    wifi_scan_list_revision_ = UINT32_MAX;
    // A field opens a full-screen editor with five wide keys per row.
    access_editor_panel_ = lv_obj_create(access_panel_);
    lv_obj_set_pos(access_editor_panel_, made_s(0), made_s(0));
    lv_obj_set_size(access_editor_panel_, made_screen_w(), made_screen_h());
    markPage(access_editor_panel_);
    lv_obj_set_style_bg_color(access_editor_panel_, lv_color_hex(0x0B1830), 0);
    lv_obj_set_style_bg_opa(access_editor_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(access_editor_panel_, 0, 0);
    lv_obj_set_style_pad_all(access_editor_panel_, 0, 0);
    lv_obj_remove_flag(access_editor_panel_, LV_OBJ_FLAG_SCROLLABLE);
    touchButton(access_editor_panel_, 78, 29, 76, 38, T("取消"), accessEditorCancelCallback, this, 0x42566B);
    touchButton(access_editor_panel_, 206, 29, 76, 38, T("完成"), accessEditorDoneCallback, this, 0x1C856F);
    access_editor_title_ = settingsLabel(access_editor_panel_, "", 55, 75, 250, 0xB8C9E4);
    access_editor_input_ = lv_textarea_create(access_editor_panel_);
    lv_obj_set_pos(access_editor_input_, made_page_x(54), made_page_y(105));
    lv_obj_set_size(access_editor_input_, made_page_w(252), made_page_h(44));
    lv_textarea_set_one_line(access_editor_input_, true);
    lv_obj_set_style_text_font(access_editor_input_, text_font, LV_PART_MAIN);
    lv_obj_add_event_cb(access_editor_input_, accessEditorDoneCallback, LV_EVENT_READY, this);
    for (lv_obj_t *field : {access_host_input_, access_port_input_,
                           access_password_input_, access_editor_input_}) {
        lv_obj_set_style_bg_color(field, lv_color_hex(0x10243B), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(field, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(field, lv_color_hex(0xF1F5FF), LV_PART_MAIN);
        lv_obj_set_style_text_color(field, lv_color_hex(0xA8BED9), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_border_color(field, lv_color_hex(0x83B5E6), LV_PART_MAIN);
        lv_obj_set_style_border_width(field, 1, LV_PART_MAIN);
    }
    lv_obj_add_flag(access_editor_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(access_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);

    phone_setup_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(phone_setup_panel_, made_screen_w(), made_screen_h());
    markPage(phone_setup_panel_);
    lv_obj_set_style_bg_color(phone_setup_panel_, lv_color_hex(0x091321), 0);
    lv_obj_set_style_bg_opa(phone_setup_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(phone_setup_panel_, 0, 0);
    lv_obj_set_style_pad_all(phone_setup_panel_, 0, 0);
    lv_obj_remove_flag(phone_setup_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(phone_setup_panel_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *phone_back = touchButton(phone_setup_panel_, 76, 28, 68, 36, "返回", phoneSetupBackCallback, this);
    lv_obj_t *phone_title = settingsLabel(phone_setup_panel_, "手机配置", 148, 37, 136);
    placePageHeader(phone_back, phone_title, nullptr);
    phone_setup_hint_ = settingsLabel(phone_setup_panel_, "", 55, 69, 250, 0x8CE4CB, false);
    phone_setup_qr_ = lv_qrcode_create(phone_setup_panel_);
    lv_qrcode_set_size(phone_setup_qr_, made_s(150));
    lv_qrcode_set_dark_color(phone_setup_qr_, lv_color_black());
    lv_qrcode_set_light_color(phone_setup_qr_, lv_color_white());
    lv_qrcode_set_quiet_zone(phone_setup_qr_, true);
    lv_obj_set_pos(phone_setup_qr_, (made_screen_w() - made_s(150)) / 2, made_page_y(108));
    lv_obj_add_flag(phone_setup_qr_, LV_OBJ_FLAG_HIDDEN);
    phone_setup_network_ = settingsLabel(phone_setup_panel_, "", 55, 281, 250, 0xE7EEFF, false);
    phone_setup_password_ = settingsLabel(phone_setup_panel_, "", 65, 302, 230, 0xE7EEFF, false);
    phone_setup_address_ = settingsLabel(phone_setup_panel_, "", 85, 323, 190, 0xB8C9E4, false);
    lv_obj_add_flag(phone_setup_panel_, LV_OBJ_FLAG_HIDDEN);

    provider_label_ = lv_label_create(screen);
    lv_obj_set_pos(provider_label_, made_x(50), made_y(108));
    lv_obj_set_style_text_align(provider_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(provider_label_, lv_color_hex(0xF1F5FF), 0);
    lv_obj_set_style_text_font(provider_label_, &lv_font_montserrat_26, 0);
    made_text(provider_label_, 260, false);

    position_label_ = lv_label_create(screen);
    lv_obj_set_pos(position_label_, made_x(50), made_y(145));
    lv_label_set_long_mode(position_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(position_label_, lv_color_hex(0x8CE4CB), 0);
    lv_obj_set_style_text_font(position_label_, text_font, 0);
    made_text(position_label_, 260, false);

    session_title_tap_ = lv_obj_create(screen);
    lv_obj_set_pos(session_title_tap_, made_x(45), made_y(140));
    lv_obj_set_size(session_title_tap_, made_s(270), made_s(26));
    lv_obj_set_style_bg_opa(session_title_tap_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(session_title_tap_, 0, 0);
    lv_obj_set_style_pad_all(session_title_tap_, 0, 0);
    lv_obj_remove_flag(session_title_tap_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(session_title_tap_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(session_title_tap_, providerSwipeCallback, LV_EVENT_GESTURE, this);
    lv_obj_add_event_cb(session_title_tap_, sessionSwipeCallback, LV_EVENT_GESTURE, this);
    lv_obj_add_event_cb(session_title_tap_, sessionTitleTapCallback, LV_EVENT_CLICKED, this);

    // This label appears only during pairing, where the six-digit code needs
    // to remain prominent. Task and speech status use the compact top line.
    meta_label_ = lv_label_create(screen);
    lv_obj_set_pos(meta_label_, made_x(65), made_y(202));
    lv_obj_set_style_text_align(meta_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(meta_label_, lv_color_hex(0xF1F5FF), 0);
    lv_obj_set_style_text_font(meta_label_, &lv_font_montserrat_26, 0);
    made_text(meta_label_, 230, false);
    lv_obj_add_flag(meta_label_, LV_OBJ_FLAG_HIDDEN);

    instruction_label_ = lv_label_create(screen);
    lv_obj_set_pos(instruction_label_, made_x(50), made_y(169));
    lv_label_set_long_mode(instruction_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(instruction_label_, lv_color_hex(0xB5CBE7), 0);
    lv_obj_set_style_text_font(instruction_label_, text_font, 0);
    made_text(instruction_label_, 260, false);

    // The answer is the main content. A transparent scroll area keeps text
    // inside the circular safe region, even when the response is long.
    answer_area_ = lv_obj_create(screen);
    lv_obj_set_pos(answer_area_, made_x(64), made_y(191));
    lv_obj_set_size(answer_area_, made_s(232), made_s(127));
    lv_obj_set_style_bg_opa(answer_area_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(answer_area_, 0, 0);
    lv_obj_set_style_pad_all(answer_area_, 5, 0);
    lv_obj_set_overflow_visible(answer_area_, true);
    lv_obj_set_scroll_dir(answer_area_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(answer_area_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(answer_area_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(answer_area_, providerSwipeCallback, LV_EVENT_GESTURE, this);

    result_label_ = lv_label_create(answer_area_);
    // Keep text inside a roughly 170 px safe radius near the bottom edge.
    lv_obj_set_pos(result_label_, made_s(13), made_s(4));
    lv_label_set_long_mode(result_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(result_label_, lv_color_hex(0xE7EEFF), 0);
    lv_obj_set_style_text_font(result_label_, text_font, 0);
    made_text(result_label_, 196, false);
    lv_obj_set_style_text_line_space(result_label_, 4, 0);
    if (made_rect() && made_fit() < made_design) {
        // The 360 coordinates leave less than one glyph between these lines.
        // Stack them at the real font height so 16 px text stays sharp and apart.
        const int margin = 6;
        const int width = made_screen_w() - margin * 2;
        const lv_font_t *title_font = &lv_font_montserrat_16;
        int y = made_y(28) + made_s(74) + 6;
        auto place = [&](lv_obj_t *label, const lv_font_t *font, int gap) {
            lv_obj_set_style_text_font(label, font, 0);
            lv_obj_set_pos(label, margin, y);
            lv_obj_set_width(label, width);
            lv_obj_set_style_transform_scale_x(label, 256, 0);
            lv_obj_set_style_transform_scale_y(label, 256, 0);
            y += lv_font_get_line_height(font) + gap;
        };
        place(provider_label_, title_font, 2);
        place(position_label_, text_font, 2);
        lv_obj_set_pos(session_title_tap_, margin, y - lv_font_get_line_height(text_font) - 2);
        lv_obj_set_size(session_title_tap_, width, lv_font_get_line_height(text_font) + 4);
        place(instruction_label_, text_font, 4);
        const int bottom = made_screen_h() - 4;
        const int line = lv_font_get_line_height(text_font);
        const int answer_h = bottom > y + line * 2 ? bottom - y : line * 3;
        lv_obj_set_pos(answer_area_, margin, y);
        lv_obj_set_size(answer_area_, width, answer_h);
        lv_obj_set_style_pad_all(answer_area_, 2, 0);
        lv_obj_set_overflow_visible(answer_area_, false);
        lv_obj_set_pos(result_label_, 0, 0);
        lv_obj_set_width(result_label_, width - 4);
        lv_obj_set_style_transform_scale_x(result_label_, 256, 0);
        lv_obj_set_style_transform_scale_y(result_label_, 256, 0);
        lv_obj_set_pos(meta_label_, margin, y);
        lv_obj_set_width(meta_label_, width);
        lv_obj_set_style_text_font(meta_label_, &lv_font_montserrat_16, 0);
        lv_obj_set_style_transform_scale_x(meta_label_, 256, 0);
        lv_obj_set_style_transform_scale_y(meta_label_, 256, 0);
    }

    // A separate list gives each discovered computer a large touch target.
    // Selection only queues worker work; discovery and pairing never block LVGL.
    bridge_picker_ = lv_obj_create(screen);
    lv_obj_set_pos(bridge_picker_, made_s(0), made_s(0));
    lv_obj_set_size(bridge_picker_, made_screen_w(), made_screen_h());
    markPage(bridge_picker_);
    lv_obj_set_style_bg_color(bridge_picker_, lv_color_hex(0x091321), 0);
    lv_obj_set_style_bg_opa(bridge_picker_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bridge_picker_, 0, 0);
    lv_obj_set_style_pad_all(bridge_picker_, 0, 0);
    lv_obj_remove_flag(bridge_picker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bridge_picker_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *bridge_settings = touchButton(bridge_picker_, 86, 29, 58, 36, "settings.short", settingsCallback, this, 0x42566B);
    lv_obj_t *bridge_title = settingsLabel(bridge_picker_, T("选择电脑"), 144, 39, 72);
    bridge_refresh_button_ = touchButton(bridge_picker_, 216, 29, 58, 36,
                                          T("刷新"), bridgeRefreshCallback, this, 0x1C856F);
    placePageHeader(bridge_settings, bridge_title, bridge_refresh_button_);
    bridge_list_ = lv_obj_create(bridge_picker_);
    lv_obj_set_pos(bridge_list_, made_page_x(45), made_page_y(88));
    lv_obj_set_size(bridge_list_, made_page_w(270), made_page_y(280) - made_page_y(88));
    lv_obj_set_style_bg_opa(bridge_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bridge_list_, 0, 0);
    lv_obj_set_style_pad_all(bridge_list_, 0, 0);
    lv_obj_set_scroll_dir(bridge_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(bridge_list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(bridge_list_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    bridge_hint_ = settingsLabel(bridge_picker_, T("正在扫描电脑…"), 76, 287, 208, 0x8CE4CB);
    lv_obj_add_flag(bridge_picker_, LV_OBJ_FLAG_HIDDEN);
    bridge_list_revision_ = UINT32_MAX;
    bridge_rows_.clear();

    delete_dialog_ = lv_obj_create(screen);
    lv_obj_set_pos(delete_dialog_, made_s(0), made_s(0));
    lv_obj_set_size(delete_dialog_, made_screen_w(), made_screen_h());
    markPage(delete_dialog_);
    lv_obj_set_style_bg_color(delete_dialog_, lv_color_hex(0x091321), 0);
    lv_obj_set_style_bg_opa(delete_dialog_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(delete_dialog_, 0, 0);
    lv_obj_set_style_pad_all(delete_dialog_, 0, 0);
    lv_obj_remove_flag(delete_dialog_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(delete_dialog_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    settingsLabel(delete_dialog_, T("删除当前任务？"), 55, 88, 250);
    delete_dialog_title_ = settingsLabel(delete_dialog_, "", 55, 129, 250, 0x8CE4CB, false);
    lv_label_set_long_mode(delete_dialog_title_, LV_LABEL_LONG_DOT);
    settingsLabel(delete_dialog_, T("任务及对话记录将被删除"), 55, 171, 250, 0x9FADD0);
    touchButton(delete_dialog_, 65, 230, 105, 50, T("返回"), deleteSessionCancelCallback, this);
    touchButton(delete_dialog_, 190, 230, 105, 50, T("删除"), deleteSessionConfirmCallback, this, 0x9C4856);
    lv_obj_add_flag(delete_dialog_, LV_OBJ_FLAG_HIDDEN);

    lock_screen_ = made_lock_screen::create(screen, lockSwipeCallback, this);
    if (!lock_screen_) {
        active_ = false;
        ESP_LOGE(BUTTON_TAG, "Cannot create Made lock screen");
        return false;
    }
    // Brookesia records timers created during run() and removes them on close.
    // The same timer animates the lock screen and refreshes the unlocked UI.
    if (!lv_timer_create(timerCallback, 50, this)) {
        active_ = false;
        ESP_LOGE(BUTTON_TAG, "Cannot start Made UI timer");
        return false;
    }
    render();
    ESP_LOGI(BUTTON_TAG, "Made lock screen ready; swipe up to connect");

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
    if (settings_open_.load()) {
        if (phone_setup_panel_ && !lv_obj_has_flag(phone_setup_panel_, LV_OBJ_FLAG_HIDDEN)) showPhoneSetup(false);
        else if (language_panel_ && !lv_obj_has_flag(language_panel_, LV_OBJ_FLAG_HIDDEN)) showLanguage(false);
        else if (access_editor_panel_ && !lv_obj_has_flag(access_editor_panel_, LV_OBJ_FLAG_HIDDEN)) closeAccessEditor(false);
        else if (wifi_picker_ && !lv_obj_has_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN)) showWifiScan(false);
        else if (receiver_picker_ && !lv_obj_has_flag(receiver_picker_, LV_OBJ_FLAG_HIDDEN)) showReceiverScan(false);
        else if (access_panel_ && !lv_obj_has_flag(access_panel_, LV_OBJ_FLAG_HIDDEN)) showAccess(false);
        else showSettings(false);
        return true;
    }
    return notifyCoreClosed();
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
    delete_dialog_open_ = false;
    if (settings_overlay_) lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);
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
    enterLockScreen();
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
    lock_screen_ = nullptr;
    language_panel_ = nullptr;
    language_zh_button_ = nullptr;
    language_en_button_ = nullptr;
    language_auto_button_ = nullptr;
    language_hint_ = nullptr;
    power_panel_ = nullptr;
    power_timeout_button_ = nullptr;
    power_timeout_label_ = nullptr;
    power_off_overlay_ = nullptr;
    provider_label_ = nullptr;
    session_title_tap_ = nullptr;
    animal_halo_ = nullptr;
    provider_image_ = nullptr;
    icon_placeholder_ = nullptr;
    lv_image_cache_drop(&provider_image_descriptor_);
    provider_image_descriptor_ = {};
    rendered_icon_ = {};
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
                if (decision == BootClickGesture::Decision::Home) {
                    // Stop the foreground loop immediately; the UI thread owns
                    // Brookesia's close event and will process it shortly.
                    exit_requested_ = true;
                    ESP_LOGI(BUTTON_TAG, "BOOT long hold: closing Vibe Coding");
                    {
                        std::lock_guard<std::mutex> lock(voice_action_mutex_);
                        vibe_voice::cancel();
                    }
                } else if (locked_.load() || settings_open_.load() || delete_dialog_open_.load()) {
                    // The settings keyboard owns touch input; no task action
                    // should be triggered by a stray BOOT click while editing.
                } else if (decision == BootClickGesture::Decision::Voice) {
                    startVoice();
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
            }
            armed_task_id.clear();
            armed_provider = -1;
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
        // Retain the request if Brookesia rejects a close during a transition;
        // the next timer tick will retry instead of silently trapping the app.
        if (!self->notifyCoreClosed()) {
            ESP_LOGW(BUTTON_TAG, "Vibe Coding close request failed; retrying");
        }
    } else {
        self->render();
    }
    self->ui_stage_.store(100);
}

void VibeCoding::enterLockScreen()
{
    locked_.store(true);
    foreground_network_pending_.store(false);
    vibe_usb::set_active(false);
    vibe_pairing::suspend_foreground();
    button_epoch_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(voice_action_mutex_);
        vibe_voice::cancel();
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        pending_action_ = {};
        pending_create_session_ = false;
        pending_delete_session_ = false;
        model_.action_in_flight = false;
        visible_task_id_.clear();
        voice_session_id_.clear();
    }
    lock_started_ms_ = lv_tick_get();
    lock_touch_tracking_ = false;
    if (lock_screen_) {
        lv_obj_remove_flag(lock_screen_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(lock_screen_);
        made_lock_screen::update(lock_screen_, 0, vibe_i18n::locale() == vibe_i18n::Locale::English);
    }
}

void VibeCoding::unlockScreen()
{
    if (!active_.load() || !locked_.load() || exit_requested_.load()) return;
    // Prepare transport before opening the worker gate. No bridge discovery,
    // heartbeat, or receiver Wi-Fi switch happens on the boot lock screen.
    vibe_pairing::begin_foreground_connection();
    foreground_network_pending_.store(true);
    button_epoch_.fetch_add(1);
    locked_.store(false);
    lock_touch_tracking_ = false;
    if (lock_screen_) lv_obj_add_flag(lock_screen_, LV_OBJ_FLAG_HIDDEN);
    drawn_revision_ = UINT32_MAX;
    ESP_LOGI(BUTTON_TAG, "Made unlocked by upward swipe");
    render();
}

void VibeCoding::lockSwipeCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->active_.load() || !self->locked_.load()) return;
    lv_event_stop_bubbling(event);
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(input, &self->lock_touch_origin_);
        self->lock_touch_tracking_ = true;
        return;
    }
    if (code == LV_EVENT_PRESS_LOST) {
        self->lock_touch_tracking_ = false;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (!self->lock_touch_tracking_) return;
        self->lock_touch_tracking_ = false;
        lv_point_t end{};
        lv_indev_get_point(input, &end);
        const int upward = self->lock_touch_origin_.y - end.y;
        const int sideways = std::abs(end.x - self->lock_touch_origin_.x);
        // Also accept a deliberate slow upward drag; LVGL's velocity threshold
        // can otherwise discard it when touch samples briefly stop moving.
        if (upward < 50 || upward * 2 <= sideways * 3) return;
    } else if (code != LV_EVENT_GESTURE || lv_indev_get_gesture_dir(input) != LV_DIR_TOP) {
        return;
    }
    // Consume the whole gesture; the page underneath must not also switch tasks.
    lv_indev_wait_release(input);
    self->unlockScreen();
}

void VibeCoding::providerSwipeCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || !self->canOperate() || self->settings_open_.load()) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) return;
    const uint32_t now = lv_tick_get();
    if (self->last_provider_swipe_ms_ != 0 && now - self->last_provider_swipe_ms_ < 350) return;
    self->last_provider_swipe_ms_ = now;
    self->chooseProvider(direction == LV_DIR_LEFT ? 1 : -1);
}

void VibeCoding::chooseProvider(int direction)
{
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
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (!config_loaded_.load() || providers_.empty()) return;
        const int count = static_cast<int>(providers_.size());
        const int current = selected_provider_.load();
        selected_provider_.store((current + direction + count) % count);
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
    lv_obj_set_style_bg_color(language_zh_button_, lv_color_hex(!automatic && !english ? 0x1C856F : 0x284668), 0);
    lv_obj_set_style_bg_color(language_en_button_, lv_color_hex(!automatic && english ? 0x1C856F : 0x284668), 0);
    lv_obj_set_style_bg_color(language_auto_button_, lv_color_hex(automatic ? 0x1C856F : 0x284668), 0);
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

void VibeCoding::powerBackCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (self) self->showPower(false);
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
        else if (self->wifi_picker_ && !lv_obj_has_flag(self->wifi_picker_, LV_OBJ_FLAG_HIDDEN))
            self->showWifiScan(false);
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
    lv_obj_set_style_bg_color(access_auto_button_,
        lv_color_hex(access_manual_selected_ || access_receiver_selected_ || access_usb_selected_ ? 0x284668 : 0x1C856F), 0);
    lv_obj_set_style_bg_color(access_manual_button_,
        lv_color_hex(access_manual_selected_ ? 0x1C856F : 0x284668), 0);
    lv_obj_set_style_bg_color(access_receiver_button_,
        lv_color_hex(access_receiver_selected_ ? 0x1C856F : 0x284668), 0);
    lv_obj_set_style_bg_color(access_usb_button_,
        lv_color_hex(access_usb_selected_ ? 0x1C856F : 0x284668), 0);
    for (lv_obj_t *item : {access_auto_button_, access_manual_button_,
                          access_receiver_button_, access_usb_button_, access_picker_hint_, phone_setup_button_}) {
        if (access_picker_open_) lv_obj_remove_flag(item, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t *item : {access_save_button_, access_change_button_, access_usb_hint_,
                          access_host_input_, access_port_input_, access_password_input_,
                          access_https_button_, receiver_scan_button_, wifi_scan_button_}) {
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
    const bool wifi_mode = !access_manual_selected_ && !access_receiver_selected_ && !access_usb_selected_;
    if (wifi_mode) lv_obj_remove_flag(wifi_scan_button_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(wifi_scan_button_, LV_OBJ_FLAG_HIDDEN);
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
        if (vibe_wifi::is_connected()) {
            wifi_ap_record_t ap = {};
            std::string status = T("已连接 Wi-Fi");
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                char ssid[33] = {};
                std::memcpy(ssid, ap.ssid, sizeof(ap.ssid));
                status += std::string("：") + ssid;
            }
            lv_label_set_text(access_status_label_, status.c_str());
        } else {
            lv_label_set_text(access_status_label_, T("尚未连接 Wi-Fi，可扫描加入网络"));
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
    // 屏幕诊断：状态行直接显示点击是否进入回调（串口抓取不可靠时的兜底）。
    if (self->wifi_scan_status_) {
        char debug[64];
        std::snprintf(debug, sizeof(debug), "clicked #%u / %u", (unsigned)index,
                      (unsigned)self->wifi_scan_snapshot_.networks.size());
        lv_label_set_text(self->wifi_scan_status_, debug);
    }
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
        if (vibe_wifi::save_credentials(ssid.c_str(), "") == ESP_OK && self->wifi_scan_status_)
            lv_label_set_text(self->wifi_scan_status_, (std::string(T("正在连接 ")) + ssid + T(" …")).c_str());
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
        lv_obj_t *hint_label = settingsLabel(wifi_network_list_, hint, 8, 10, 254, 0xB8C9E4, false);
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
                                       nullptr, wifiNetworkCallback, this, 0x10243B);
        lv_obj_t *name = settingsLabel(button, network.ssid.c_str(), 10, 4, 234, 0xF1F5FF, false);
        lv_obj_set_style_text_font(name, text_font, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *sub_label = settingsLabel(button, sub, 10, 26, 234, 0xB8C9E4, false);
        lv_obj_set_style_text_font(sub_label, text_font, 0);
        lv_obj_remove_flag(sub_label, LV_OBJ_FLAG_CLICKABLE);
        ++row;
    }
}

void VibeCoding::showWifiScan(bool visible)
{
    if (!wifi_picker_) return;
    if (!visible) {
        lv_obj_add_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_picker_);
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
        lv_obj_t *name = settingsLabel(button, network.ssid.c_str(), 10, 7, 234, 0xF1F5FF, false);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_set_height(name, made_s(20));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        const std::string strength = std::to_string(network.rssi) + " dBm";
        lv_obj_t *signal = settingsLabel(button, strength.c_str(), 10, 31, 234, 0xB8C9E4, false);
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
        saved = vibe_pairing::use_automatic_discovery();
        if (!saved) error = T("保存自动发现设置失败");
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

void VibeCoding::startVoice()
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
        }
        if (!provider_ready) {
            hint = "Selected coding agent is unavailable";
        } else if (session_id.empty()) {
            hint = T("点右上角 + 创建任务");
        } else if (project_id.empty()) {
            hint = T("当前任务没有项目，请在电脑端设置");
        } else {
            if (vibe_voice::start(pairing.url, pairing.token,
                                  provider_id, project_id, session_id)) {
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
        lv_obj_t *name = settingsLabel(button, bridge.name.c_str(), 12, 6, 230, 0xE7EEFF, false);
        lv_obj_set_height(name, made_s(20));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        const size_t scheme = bridge.url.find("://");
        const std::string address = bridge.url.substr(scheme == std::string::npos ? 0 : scheme + 3);
        lv_obj_t *host = settingsLabel(button, address.c_str(), 12, 28, 230, 0xB8C9E4, false);
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

void VibeCoding::noteComputerReply(const Task &task)
{
    const bool pending = task.status == "queued" || task.status == "running" ||
                         task.status == "waiting_confirmation";
    const bool arrived = (task.status == "completed" || task.status == "failed" ||
                          task.status == "handed_off") &&
                         (!task.result.empty() || !task.error.empty());
    if (!reply_watch_ready_) {
        reply_watch_ready_ = true;
        reply_watch_id_ = task.id;
        reply_saw_pending_ = pending;
        reply_wait_new_ = false;
        return;
    }
    if (task.id != reply_watch_id_) {
        const bool expecting = reply_saw_pending_ || reply_wait_new_;
        reply_watch_id_ = task.id;
        reply_saw_pending_ = pending;
        reply_wait_new_ = false;
        if (arrived && expecting) made_chime_new_message();
        return;
    }
    if (pending) reply_saw_pending_ = true;
    else if (arrived && reply_saw_pending_) {
        reply_saw_pending_ = false;
        reply_wait_new_ = false;
        made_chime_new_message();
    }
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
    if (locked_.load()) {
        if (lock_screen_) made_lock_screen::update(lock_screen_, lv_tick_get() - lock_started_ms_,
            vibe_i18n::locale() == vibe_i18n::Locale::English);
        return;
    }
    if (!instruction_label_) return;
    const auto voice = vibe_voice::status();
    ui_stage_.store(3);
    const auto pairing = vibe_pairing::snapshot();
    ui_stage_.store(4);
    refreshLanguage();
    renderBridgeChoices(pairing);
    renderReceiverScan();
    renderWifiNetworks();
    renderPhoneSetup();
    const bool catalog_ready = config_loaded_.load() &&
        pairing.phase == vibe_pairing::Phase::Paired && vibe_pairing::authorized_for_foreground();
    const int voice_phase = static_cast<int>(voice.phase);
    int provider_index = 0;
    size_t provider_count = 0;
    vibe_provider::Provider provider;
    Snapshot snapshot;
    bool provider_ready = true;
    std::string selected_session_id;
    std::string voice_session_id;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (rendered_catalog_ready_ == catalog_ready && drawn_revision_ == model_.revision && last_pair_revision_ == pairing.revision &&
            voice_phase == last_voice_phase_ && voice.message == last_voice_message_) return;
        snapshot = model_;
        provider_index = selected_provider_.load();
        provider_count = providers_.size();
        provider = selectedProviderLocked();
        provider_ready = provider.available;
        selected_session_id = provider.selected_session_id;
        voice_session_id = voice_session_id_;
    }
    const std::string own_error = vibe_i18n::message(snapshot.connection_error);
    const std::string own_feedback = snapshot.feedback_external ? vibe_i18n::transport_error(snapshot.feedback) : vibe_i18n::message(snapshot.feedback);
    const std::string own_voice_hint = vibe_i18n::message(snapshot.voice_hint);
    const std::string own_session_notice = vibe_i18n::message(snapshot.session_notice);
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

    const bool ascii_label = std::all_of(provider.label.begin(), provider.label.end(),
        [](unsigned char c) { return c < 128; });
    lv_obj_set_style_text_font(provider_label_, ascii_label ?
        (made_fit() < made_design ? &lv_font_montserrat_16 : &lv_font_montserrat_26) : text_font, 0);
    lv_label_set_long_mode(provider_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(provider_label_, provider.label.c_str());
    const std::string page_number = std::to_string(provider_count ? provider_index + 1 : 0) + " / " + std::to_string(provider_count);
    lv_label_set_text(page_label_, page_number.c_str());
    if (catalog_ready && provider_count) {
        lv_obj_remove_flag(animal_halo_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page_label_, LV_OBJ_FLAG_HIDDEN);
        showProviderIcon(provider.icon);
    } else {
        lv_obj_add_flag(animal_halo_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(provider_label_, text_font, 0);
        lv_label_set_text(provider_label_, catalog_ready ? T("暂无助手") : T("连接电脑"));
    }

    if (pairing.phase != vibe_pairing::Phase::Paired ||
        !vibe_pairing::authorized_for_foreground()) {
        lv_obj_add_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
        displayed_task_id_.clear();
        if (!showing_pairing_layout_) {
            lv_obj_remove_flag(meta_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(answer_area_, made_x(64), made_y(245));
            lv_obj_set_size(answer_area_, made_s(232), made_s(74));
            lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_CENTER, 0);
            showing_pairing_layout_ = true;
        }
        const size_t scheme = pairing.url.find("://");
        const std::string target = pairing.url.empty() ? std::string() :
            pairing.url.substr(scheme == std::string::npos ? 0 : scheme + 3);
        const bool usb_direct = pairing.access_mode == vibe_pairing::AccessMode::UsbDirect;
        const std::string computer = usb_direct ? T("USB 直连电脑") :
            target.empty() ? T("正在查找电脑") : T("目标 ") + target;
        lv_label_set_text(position_label_, computer.c_str());
        lv_label_set_text(instruction_label_, pairing.phase == vibe_pairing::Phase::WaitingApproval ?
                          T("六位配对码") : T("正在查找电脑"));
        lv_label_set_text(meta_label_, pairing.code.empty() ? "------" : pairing.code.c_str());
        const std::string pairing_message = snapshot.connection_error == "Connect receiver hotspot first" ?
            T("请核对接收端热点，确认接收端已插入电脑") :
            snapshot.connection_error == "Connect Wi-Fi first" ?
            T("请先在设置里连接 Wi-Fi") : pairing.phase == vibe_pairing::Phase::WaitingApproval ?
            T("在电脑页面核对号码并确认") : pairing.retry_required ? T("单击 BOOT 重新配对") : pairing.message;
        showAnswer("pairing", usb_direct || target.empty() ? pairing_message :
                   T("目标：") + target + "\n" + pairing_message);
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            visible_task_id_.clear();
        }
        layoutReadableText();
        drawn_revision_ = snapshot.revision;
        return;
    }

    if (catalog_ready && provider_count) lv_obj_remove_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
    if (showing_pairing_layout_) {
        lv_obj_add_flag(meta_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(answer_area_, made_x(64), made_y(191));
        lv_obj_set_size(answer_area_, made_s(232), made_s(127));
        lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_LEFT, 0);
        showing_pairing_layout_ = false;
    }
    if (!catalog_ready || !provider_count) {
        lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
        displayed_task_id_.clear();
        rendered_session_id_.clear();
        lv_label_set_text(position_label_, catalog_ready ? T("暂无助手") : T("读取助手列表"));
        lv_label_set_text(instruction_label_, catalog_ready ? T("请在电脑端添加助手插件") : T("正在读取电脑设置"));
        showAnswer("catalog", !snapshot.connection_error.empty() ? own_error :
            catalog_ready ? T("助手名称和图标将由电脑下发。") : T("连接成功后会自动显示可用助手。"));
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            visible_task_id_.clear();
        }
        layoutReadableText();
        drawn_revision_ = snapshot.revision;
        return;
    }
    const bool voice_for_selected_session = !selected_session_id.empty() &&
        voice_session_id == selected_session_id;
    const bool show_voice_status = voice_for_selected_session &&
        (voice.phase == vibe_voice::Phase::Recording ||
         voice.phase == vibe_voice::Phase::Uploading ||
         (voice.phase == vibe_voice::Phase::Submitted && !submitted_task_seen_.load() && snapshot.tasks.empty()) ||
         voice.phase == vibe_voice::Phase::Error);
    const std::string voice_notice = show_voice_status && !voice.message.empty() ? voice.message :
        !snapshot.voice_hint.empty() ? own_voice_hint : "";

    const auto selected_session = std::find_if(snapshot.sessions.begin(), snapshot.sessions.end(),
        [&selected_session_id](const Session &session) { return session.id == selected_session_id; });
    if (selected_session == snapshot.sessions.end()) lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
    if (selected_session_id != rendered_session_id_) {
        rendered_session_id_ = selected_session_id;
        displayed_task_id_.clear();
    }
    if (selected_session == snapshot.sessions.end()) {
        displayed_task_id_.clear();
        lv_label_set_text(position_label_, T("暂无任务"));
        lv_label_set_text(instruction_label_, T("点右上角 + 创建任务"));
        const std::string empty_message = !snapshot.connection_error.empty() ? own_error :
            !snapshot.session_notice.empty() ? own_session_notice :
            !provider_ready ? (provider.reason.empty() ? T("请先在电脑端启用此助手") : provider.reason) :
            T("先创建任务，再单击 BOOT 说话。左右滑动换助手。");
        showAnswer(provider.id + "/no-session", empty_message);
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            visible_task_id_.clear();
        }
        layoutReadableText();
        drawn_revision_ = snapshot.revision;
        return;
    }
    const std::string session_header = T("任务 ") +
        std::to_string(static_cast<int>(selected_session - snapshot.sessions.begin()) + 1) + "/" +
        std::to_string(snapshot.sessions.size()) + " · " +
        (selected_session->title.empty() ? T("未命名任务") : selected_session->title);
    lv_label_set_text(position_label_, session_header.c_str());
    ui_stage_.store(20);

    if (voice_for_selected_session && voice.phase == vibe_voice::Phase::Submitted && !voice.task_id.empty() &&
        !submitted_task_seen_.load()) {
        const auto submitted = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
            [&voice](const Task &task) { return task.id == voice.task_id; });
        if (submitted == snapshot.tasks.end()) {
            // Do not leave an older actionable exchange on screen after the 201.
            displayed_task_id_.clear();
            lv_label_set_text(instruction_label_, T("新一轮已发送"));
            showAnswer(selected_session_id + "/submitted", T("正在读取刚发送的内容。三击 BOOT 可取消，双击请等回显。"));
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                visible_task_id_.clear();
            }
            layoutReadableText();
            drawn_revision_ = snapshot.revision;
            return;
        }
    }

    if (snapshot.tasks.empty()) {
        displayed_task_id_.clear();
        lv_label_set_text(instruction_label_, !voice_notice.empty() ? voice_notice.c_str() :
                          provider_ready ? T("还没有对话 · 单击 BOOT 说话") : T("电脑端助手不可用"));
        const std::string result = !snapshot.connection_error.empty() ? own_error :
            !config_loaded_.load() ? T("正在读取电脑设置") :
            !voice_available_.load() ? T("在电脑工作台设置语音识别") :
            voice.phase == vibe_voice::Phase::Recording ? T("说完后稍等，会自动发送。") :
            voice.phase == vibe_voice::Phase::Uploading ? T("正在识别语音…") :
            !snapshot.session_notice.empty() ? own_session_notice :
            !voice_notice.empty() ? voice_notice :
            provider.external_unscoped ? T("此助手任务隔离受限，请在电脑端核对。顶部上下滑切任务。") :
            T("顶部上下滑切任务，点标题也可切换。左右滑切助手。");
        showAnswer(selected_session_id + "/empty", result);
    } else {
        // The bridge returns newest first. Show only the latest exchange.
        const Task &task = snapshot.tasks.front();
        displayed_task_id_ = task.id;
        noteComputerReply(task);
        const std::string instruction = !voice_notice.empty() ? voice_notice :
            std::string(statusName(task.status)) +
            T(" · 你：") + task.instruction;
        std::string result;
        bool showing_task_result = false;
        if (task.status == "waiting_confirmation") {
            // The short header is only a preview. A user must be able to read
            // the entire recognized instruction before the BOOT double-click.
            const std::string notice = task.instruction_truncated ?
                T("任务过长，请在电脑任务页核对并确认") :
                !snapshot.connection_error.empty() ? own_error :
                (snapshot.feedback_task_id == task.id && !snapshot.feedback.empty()) ? own_feedback :
                !task.error.empty() ? task.error : T("双击 BOOT 确认 · 三击取消");
            result = notice + "\n" + (task.instruction.empty() ? T("（无任务内容）") : task.instruction);
        } else if (!snapshot.connection_error.empty()) result = own_error;
        else if (snapshot.feedback_task_id == task.id && !snapshot.feedback.empty()) result = own_feedback;
        else if (!task.error.empty()) result = task.error;
        else if (task.status == "running" && !task.progress.empty()) result = task.progress;
        else if (task.status == "queued") result = T("已排队，等待助手启动");
        else {
            result = task.result.empty() ? T("等待助手回复") : task.result;
            showing_task_result = true;
        }
        if (showing_task_result && task.result_truncated) {
            result += T("\n\n更多内容请在电脑任务页查看");
        }
        lv_label_set_text(instruction_label_, instruction.c_str());
        ui_stage_.store(30);
        showAnswer(selected_session_id + "/" + task.id + "/" + task.status,
                   result);
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        visible_task_id_ = displayed_task_id_;
        if (voice.phase == vibe_voice::Phase::Submitted && visible_task_id_ == voice.task_id) {
            submitted_task_seen_ = true;
        }
    }
    layoutReadableText();
    drawn_revision_ = snapshot.revision;
}

void VibeCoding::layoutReadableText()
{
    if (!made_rect() || made_fit() >= made_design || !provider_label_ || !position_label_ ||
        !instruction_label_ || !answer_area_ || !result_label_) return;
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

esp_err_t VibeCoding::httpEvent(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || event->data_len <= 0) return ESP_OK;
    auto *response = static_cast<std::string *>(event->user_data);
    if (response->size() + static_cast<size_t>(event->data_len) > MAX_RESPONSE_BYTES) return ESP_FAIL;
    response->append(static_cast<const char *>(event->data), event->data_len);
    return ESP_OK;
}

bool VibeCoding::request(const std::string &path, bool post, std::string &response, int &status)
{
    if (locked_.load()) return false;
    if (!post && receiverVoiceUploading()) return false;
    const auto pairing = vibe_pairing::snapshot();
    std::string base = pairing.url;
    const std::string token = pairing.token;
    if (base.empty() || token.empty() || !vibe_pairing::authorized_for_foreground()) return false;
    if (base.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) {
        setConnectionError(vibe_pairing::tls_time_failed() ?
            T("设备时间同步失败，无法校验 HTTPS 证书") : T("正在同步设备时间，以校验 HTTPS 证书"));
        return false;
    }
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string wire_path = vibe_i18n::request_path(path);
    const std::string url = base + wire_path;
    const std::string authorization = "Bearer " + token;
    esp_err_t error = ESP_FAIL;
    if (pairing.access_mode == vibe_pairing::AccessMode::UsbDirect) {
        error = vibe_usb::request(wire_path, post, authorization, "", nullptr, 0,
                                  response, status, MAX_RESPONSE_BYTES, 5000);
    } else {
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.disable_auto_redirect = true;
        if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
        config.event_handler = httpEvent;
        config.user_data = &response;
        config.timeout_ms = 5000;
        config.buffer_size = 1024;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return false;
        esp_http_client_set_header(client, "ngrok-skip-browser-warning", "1");
        esp_http_client_set_header(client, "Authorization", authorization.c_str());
        if (post) esp_http_client_set_method(client, HTTP_METHOD_POST);
        error = esp_http_client_perform(client);
        status = error == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);
    }
    const auto latest = vibe_pairing::snapshot();
    if (!vibe_pairing::authorized_for_foreground() || latest.url != pairing.url ||
        latest.token != token) {
        status = 0;
        return false; // Discard a reply from the previous access mode.
    }
    if (error != ESP_OK) {
        invalidateCatalog(); // Re-read the catalog as soon as the bridge reconnects.
        vibe_pairing::reconnect();
    }
    return error == ESP_OK;
}

void VibeCoding::invalidateCatalog(bool clear)
{
    std::lock_guard<std::mutex> lock(model_mutex_);
    catalog_epoch_.fetch_add(1);
    config_loaded_.store(false);
    voice_available_.store(false);
    visible_task_id_.clear();
    if (clear) {
        providers_.clear();
        selected_provider_.store(0);
        catalog_identity_.clear();
        model_.sessions.clear();
        model_.tasks.clear();
        model_.feedback.clear();
        model_.feedback_task_id.clear();
        model_.session_notice.clear();
        voice_session_id_.clear();
        pending_action_ = {};
        model_.action_in_flight = false;
        pending_create_session_ = false;
        pending_create_provider_id_.clear();
        pending_create_project_id_.clear();
    }
    ++model_.revision;
}

// Called only by render() on the LVGL task. Network workers keep packed pixels
// in the catalog and never touch LVGL objects, descriptors or this pixel buffer.
void VibeCoding::showProviderIcon(const vibe_provider::Icon &icon)
{
    if (icon == rendered_icon_) return;
    lv_obj_invalidate(provider_image_);
    lv_image_cache_drop(&provider_image_descriptor_);
    rendered_icon_ = icon;
    lv_obj_set_style_bg_color(animal_halo_, lv_color_hex(icon.background), 0);
    lv_obj_set_style_border_color(animal_halo_, lv_color_hex(icon.accent), 0);
    if (!vibe_provider::decodeIconCanvasBgra(icon, provider_image_pixels_.data(), provider_image_pixels_.size())) {
        lv_obj_add_flag(provider_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(icon_placeholder_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    provider_image_descriptor_ = {};
    provider_image_descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    provider_image_descriptor_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    provider_image_descriptor_.header.w = vibe_provider::MAX_ICON_SIDE;
    provider_image_descriptor_.header.h = vibe_provider::MAX_ICON_SIDE;
    provider_image_descriptor_.header.stride = vibe_provider::MAX_ICON_SIDE * 4;
    provider_image_descriptor_.data_size = provider_image_pixels_.size();
    provider_image_descriptor_.data = provider_image_pixels_.data();
    lv_image_set_src(provider_image_, &provider_image_descriptor_);
    lv_image_set_scale(provider_image_, 64 * LV_SCALE_NONE / vibe_provider::MAX_ICON_SIDE);
    lv_obj_center(provider_image_);
    lv_obj_remove_flag(provider_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(icon_placeholder_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(provider_image_);
}

void VibeCoding::refreshConfig()
{
    const uint32_t epoch = catalog_epoch_.load();
    const auto origin = vibe_pairing::snapshot();
    std::string response;
    int status = 0;
    if (!request("/device/config", false, response, status)) return;
    if (receiverVoiceUploading()) return;
    if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        return;
    }
    if (status != 200) { invalidateCatalog(); return; }
    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) { invalidateCatalog(); return; }

    std::vector<vibe_provider::Provider> providers;
    const bool has_catalog = vibe_provider::parse(cJSON_GetObjectItemCaseSensitive(root, "providers"), providers);
    const cJSON *voice = cJSON_GetObjectItemCaseSensitive(root, "voice");
    const bool can_voice = cJSON_IsObject(voice) &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(voice, "available"));
    const std::string project = jsonString(root, "defaultProject");
    const std::string received_locale = jsonString(root, "locale");
    cJSON_Delete(root);
    if (!has_catalog) { invalidateCatalog(); return; }
    const auto latest = vibe_pairing::snapshot();
    if (!canOperate() || !vibe_pairing::authorized_for_foreground() ||
        origin.url != latest.url || origin.token != latest.token) return;

    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto authenticated = vibe_pairing::snapshot();
        if (epoch != catalog_epoch_.load() || !canOperate() ||
            !vibe_pairing::authorized_for_foreground() || authenticated.url != origin.url ||
            authenticated.token != origin.token) return;
        if (!received_locale.empty()) (void)vibe_i18n::apply_bridge_locale(received_locale.c_str());
        const std::string identity = origin.url + "\n" + origin.token;
        if (identity != catalog_identity_) {
            providers_.clear();
            catalog_identity_ = identity;
            model_.sessions.clear();
            model_.tasks.clear();
            visible_task_id_.clear();
        }
        if (has_catalog) {
            const auto selected_id = selectedProviderLocked().id;
            const int previous_index = selected_provider_.load();
            const auto index = vibe_provider::replace(providers_, std::move(providers), selected_id);
            selected_provider_.store(static_cast<int>(index));
            // With no providers there is no session fetch to clear stale errors.
            if (providers_.empty()) model_.connection_error.clear();
            // Only an actual selection/index change invalidates BOOT gestures.
            if (previous_index != static_cast<int>(index) || selectedProviderLocked().id != selected_id)
                button_epoch_.fetch_add(1);
            if (selectedProviderLocked().id != selected_id) {
                model_.sessions.clear();
                model_.tasks.clear();
                model_.feedback.clear();
                model_.feedback_task_id.clear();
                model_.session_notice.clear();
                visible_task_id_.clear();
            }
        }
        voice_project_id_ = project;
        if (can_voice) model_.voice_hint.clear();
        voice_available_.store(can_voice);
        config_loaded_.store(true);
        ++model_.revision;
    }
}

void VibeCoding::sendHeartbeat()
{
    std::string response;
    int status = 0;
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const std::string path = "/device/heartbeat?uiStage=" + std::to_string(ui_stage_.load()) +
        "&uiAge=" + std::to_string(now_ms - ui_tick_ms_.load()) +
        "&bootAge=" + std::to_string(now_ms - boot_tick_ms_.load()) +
        "&exit=" + std::to_string(exit_requested_.load()) +
        "&displayStage=" + std::to_string(display_stage_.load()) +
        "&displayCount=" + std::to_string(display_count_.load()) +
        "&provider=" + std::to_string(selected_provider_.load());
    const bool sent = request(path, false, response, status);
    if (receiverVoiceUploading()) return;
    if (!sent) {
        setConnectionError("Computer bridge offline");
        return;
    }
    if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        setConnectionError("Computer unpaired this device");
        return;
    }
    if (status != 200) {
        setConnectionError("Bridge heartbeat HTTP " + std::to_string(status));
        return;
    }
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (model_.connection_error == "Computer bridge offline" ||
        model_.connection_error.rfind("Bridge heartbeat HTTP ", 0) == 0) {
        model_.connection_error.clear();
        ++model_.revision;
    }
}

void VibeCoding::refreshSessions()
{
    std::string provider_id;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        provider_id = selectedProviderLocked().id;
    }
    if (!vibe_provider::validId(provider_id)) return;
    std::string response;
    int status = 0;
    const bool sent = request(std::string("/device/sessions?provider=") + provider_id,
                              false, response, status);
    if (receiverVoiceUploading()) return;
    if (!sent) {
        setConnectionError("Bridge unreachable");
        return;
    }
    if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        setConnectionError("Computer unpaired this device");
        return;
    }
    if (status != 200) {
        setConnectionError("Bridge sessions HTTP " + std::to_string(status));
        return;
    }
    cJSON *root = cJSON_Parse(response.c_str());
    const cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "sessions") : nullptr;
    if (!cJSON_IsArray(items)) {
        if (root) cJSON_Delete(root);
        setConnectionError("Invalid session list");
        return;
    }
    std::vector<Session> sessions;
    const int count = std::min(cJSON_GetArraySize(items), 32);
    sessions.reserve(count);
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) continue;
        Session session = {
            jsonString(item, "id"),
            jsonString(item, "title"),
            jsonString(item, "provider"),
            jsonString(item, "projectId"),
        };
        if (validSessionId(session.id) && session.provider == provider_id &&
            validProjectId(session.project_id)) {
            sessions.push_back(std::move(session));
        }
    }
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (selectedProviderLocked().id != provider_id) return;
        auto *provider = findProviderLocked(provider_id);
        if (!provider) return;
        auto &selected_id = provider->selected_session_id;
        const bool selected_exists = std::any_of(sessions.begin(), sessions.end(),
            [&selected_id](const Session &session) { return session.id == selected_id; });
        const std::string next_id = selected_exists ? selected_id :
            (sessions.empty() ? "" : sessions.front().id);
        if (selected_id != next_id) {
            selected_id = next_id;
            model_.tasks.clear();
            model_.feedback.clear();
            model_.feedback_task_id.clear();
            visible_task_id_.clear();
        }
        model_.sessions = std::move(sessions);
        model_.connection_error.clear();
        ++model_.revision;
    }
}

void VibeCoding::createSession(const std::string &provider_id, const std::string &project_id)
{
    if (!vibe_provider::validId(provider_id) || !validProjectId(project_id)) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        create_session_in_flight_ = false;
        model_.session_notice = T("新建任务参数无效");
        ++model_.revision;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto *provider = findProviderLocked(provider_id);
        if (!provider || !provider->available) {
            create_session_in_flight_ = false;
            model_.session_notice = T("此助手已移除或不可用");
            ++model_.revision;
            return;
        }
    }
    std::string response;
    int status = 0;
    const std::string path = std::string("/device/sessions?provider=") +
        provider_id + "&projectId=" + project_id;
    const bool sent = request(path, true, response, status);
    std::string notice;
    Session created;
    if (sent && (status == 200 || status == 201)) {
        cJSON *root = cJSON_Parse(response.c_str());
        if (root) {
            created = {jsonString(root, "id"), jsonString(root, "title"),
                       jsonString(root, "provider"), jsonString(root, "projectId")};
            cJSON_Delete(root);
        }
        if (!validSessionId(created.id) || created.provider != provider_id ||
            created.project_id != project_id) {
            created = {};
            notice = T("新建任务响应无效");
        }
    } else if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        notice = T("电脑已解除配对");
    } else {
        notice = sent ? T("新建任务失败 (HTTP ") + std::to_string(status) + ")" : T("电脑连接失败");
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        create_session_in_flight_ = false;
        auto *provider = findProviderLocked(provider_id);
        if (!created.id.empty() && provider) {
            provider->selected_session_id = created.id;
            if (selectedProviderLocked().id == provider_id) {
                model_.sessions.insert(model_.sessions.begin(), std::move(created));
                model_.tasks.clear();
                model_.feedback.clear();
                model_.feedback_task_id.clear();
                visible_task_id_.clear();
                model_.session_notice = provider->external_unscoped ?
                    T("此助手任务上下文可能共享，请在电脑端核对") :
                    T("任务已创建，单击 BOOT 说话");
            }
        } else if (selectedProviderLocked().id == provider_id) {
            model_.session_notice = std::move(notice);
        }
        ++model_.revision;
    }
}

void VibeCoding::deleteSession(const std::string &provider_id, const std::string &session_id)
{
    std::string response;
    int status = 0;
    const bool sent = validSessionId(session_id) && vibe_provider::validId(provider_id) &&
        request(std::string("/device/sessions/") + session_id + "/delete", true, response, status);
    bool deleted = false;
    std::string notice;
    cJSON *root = sent ? cJSON_Parse(response.c_str()) : nullptr;
    if (sent && status == 200 && root && jsonString(root, "id") == session_id &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "deleted"))) {
        deleted = true;
        notice = T("任务已删除");
    } else if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        notice = T("电脑已解除配对");
    } else if (!sent) {
        notice = T("电脑连接失败");
    } else if (root && !jsonString(root, "error").empty()) {
        notice = jsonString(root, "error");
    } else {
        notice = T("删除任务失败 (HTTP ") + std::to_string(status) + ")";
    }
    if (root) cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        delete_session_in_flight_ = false;
        auto *provider = findProviderLocked(provider_id);
        if (deleted && provider && provider->selected_session_id == session_id)
            provider->selected_session_id.clear();
        if (selectedProviderLocked().id == provider_id) {
            if (deleted) {
                model_.sessions.erase(std::remove_if(model_.sessions.begin(), model_.sessions.end(),
                    [&session_id](const Session &item) { return item.id == session_id; }), model_.sessions.end());
                model_.tasks.clear();
                model_.feedback.clear();
                model_.feedback_task_id.clear();
                visible_task_id_.clear();
                voice_session_id_.clear();
            }
            model_.session_notice = std::move(notice);
            ++model_.revision;
        }
    }
}

void VibeCoding::refreshTasks()
{
    std::string provider_id;
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto &provider = selectedProviderLocked();
        provider_id = provider.id;
        session_id = provider.selected_session_id;
    }
    if (!validSessionId(session_id)) return;
    std::string response;
    int status = 0;
    const bool sent = request(std::string("/device/tasks?provider=") + provider_id +
                              "&sessionId=" + session_id, false, response, status);
    if (receiverVoiceUploading()) return;
    if (!sent) {
        setConnectionError("Bridge unreachable");
        return;
    }
    if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        setConnectionError("Computer unpaired this device");
        return;
    }
    if (status != 200) {
        setConnectionError("Bridge HTTP " + std::to_string(status));
        return;
    }
    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) {
        setConnectionError("Invalid bridge response");
        return;
    }
    const cJSON *jobs = cJSON_GetObjectItemCaseSensitive(root, "jobs");
    if (!cJSON_IsArray(jobs)) {
        cJSON_Delete(root);
        setConnectionError("Invalid task list");
        return;
    }
    std::vector<Task> tasks;
    const int count = std::min(cJSON_GetArraySize(jobs), 8);
    tasks.reserve(1);
    for (int i = 0; i < count; ++i) {
        const cJSON *job = cJSON_GetArrayItem(jobs, i);
        if (!cJSON_IsObject(job)) continue;
        Task task = {
            jsonString(job, "id"),
            jsonString(job, "instruction"),
            jsonString(job, "provider"),
            jsonString(job, "projectId"),
            jsonString(job, "status"),
            jsonString(job, "result"),
            jsonString(job, "progress"),
            jsonString(job, "error"),
            jsonString(job, "vibeSessionId"),
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "resultTruncated")) != 0,
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "instructionTruncated")) != 0,
        };
        if (validTaskId(task.id) && task.provider == provider_id &&
            task.session_id == session_id) {
            tasks.push_back(std::move(task));
            break; // The current task shows its newest exchange only.
        }
    }
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (selectedProviderLocked().id != provider_id ||
            selectedProviderLocked().selected_session_id != session_id) return;
        model_.tasks = std::move(tasks);
        model_.connection_error.clear();
        model_.feedback_external = false;
        if (model_.feedback == "Confirmed" || model_.feedback == "Cancelled" ||
            std::none_of(model_.tasks.begin(), model_.tasks.end(), [this](const Task &task) {
                return task.id == model_.feedback_task_id &&
                    (task.status == "waiting_confirmation" || task.status == "queued" || task.status == "running");
            })) {
            model_.feedback.clear();
            model_.feedback_task_id.clear();
        }
        ++model_.revision;
    }
}

void VibeCoding::sendAction(PendingAction action)
{
    std::string response;
    int status = 0;
    const bool sent = request("/device/tasks/" + action.id + "/" + action.verb, true, response, status);
    std::string feedback;
    bool external_feedback = false;
    if (!sent) {
        feedback = "Action could not reach bridge";
    } else if (status == 200) {
        feedback = action.verb == "confirm" ? "Confirmed" : "Cancelled";
    } else {
        cJSON *root = cJSON_Parse(response.c_str());
        feedback = root ? jsonString(root, "error") : "";
        external_feedback = !feedback.empty();
        if (root) cJSON_Delete(root);
        if (feedback.empty()) feedback = "Action rejected (HTTP " + std::to_string(status) + ")";
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.feedback_external = external_feedback;
        model_.feedback = std::move(feedback);
        model_.feedback_task_id = action.id;
        model_.action_in_flight = false;
        ++model_.revision;
    }
}

void VibeCoding::workerEntry(void *arg)
{
    static_cast<VibeCoding *>(arg)->workerLoop();
}

void VibeCoding::ensureWorkerStarted()
{
    bool expected = false;
    if (worker_started_.compare_exchange_strong(expected, true) &&
        xTaskCreate(workerEntry, "vibe_bridge", 12288, this, 3, nullptr) != pdPASS) {
        worker_started_ = false;
        setConnectionError("Cannot start network task");
    }
}

void VibeCoding::workerLoop()
{
    uint32_t active_cycles = 0;
    uint64_t last_heartbeat_ms = 0;
    uint32_t seen_app_epoch = button_epoch_.load();
    bool phone_setup_started = false;
    uint32_t phone_epoch = 0;
    while (true) {
        const uint32_t app_epoch = button_epoch_.load();
        if (app_epoch != seen_app_epoch) {
            seen_app_epoch = app_epoch;
            last_heartbeat_ms = 0;
        }
        bool has_pending_action = false;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            has_pending_action = !pending_action_.verb.empty();
        }
        const bool foreground = canOperate();
        const uint32_t requested_phone_epoch = phone_setup_epoch_.load();
        const bool want_phone = foreground && !exit_requested_.load() && phone_setup_requested_.load();
        // The portal must run before the Wi-Fi readiness gates, including when
        // the device has never joined a network. All blocking cleanup stays here.
        if ((phone_setup_started || vibe_wifi::setup_ap_active()) && (!want_phone || requested_phone_epoch != phone_epoch)) {
            vibe_phone_setup::stop();
            phone_setup_started = false;
            if (vibe_wifi::setup_ap_active()) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }
        if (restore_station_pending_.load() && (!active_.load() || locked_.load()) &&
            (!vibe_wifi::receiver_active() || vibe_wifi::restore_station() == ESP_OK))
            restore_station_pending_.store(false);
        if (want_phone) {
            auto fail_phone = [this, requested_phone_epoch](const std::string &message) {
                if (requested_phone_epoch != phone_setup_epoch_.load() ||
                    !phone_setup_requested_.load() || !active_.load()) return;
                { std::lock_guard<std::mutex> lock(model_mutex_); phone_setup_error_ = message; }
                phone_setup_requested_.store(false);
                phone_setup_result_.store(-1);
            };
            if (!phone_setup_started) {
                phone_epoch = requested_phone_epoch;
                vibe_phone_setup::InitialConfig initial;
                const auto pairing = vibe_pairing::snapshot();
                initial.mode = pairing.access_mode == vibe_pairing::AccessMode::Manual ? "manual" :
                               pairing.access_mode == vibe_pairing::AccessMode::Receiver ? "receiver" : "automatic";
                char receiver_ssid[33] = {};
                if (vibe_wifi::receiver_ssid(receiver_ssid, sizeof(receiver_ssid))) initial.receiver_ssid = receiver_ssid;
                initial.https = pairing.manual_url.empty() || pairing.manual_url.rfind("https://", 0) == 0;
                initial.port = initial.https ? 443 : 8788;
                if (!pairing.manual_url.empty()) {
                    const size_t scheme = pairing.manual_url.find("://");
                    const std::string authority = scheme == std::string::npos ? pairing.manual_url : pairing.manual_url.substr(scheme + 3);
                    const size_t colon = authority.rfind(':');
                    initial.host = colon == std::string::npos ? authority : authority.substr(0, colon);
                    if (colon != std::string::npos) initial.port = static_cast<uint16_t>(std::strtoul(authority.substr(colon + 1).c_str(), nullptr, 10));
                }
                const esp_err_t result = vibe_phone_setup::start(vibe_i18n::locale() == vibe_i18n::Locale::English, initial);
                phone_setup_started = true; // Also clean up a partially started portal.
                phone_setup_ready_epoch_.store(phone_epoch);
                if (result != ESP_OK) fail_phone(T("手机配置开启失败，请返回重试"));
            }
            // Back / long BOOT / app close may have happened during startup.
            if (!active_.load() || exit_requested_.load() || !phone_setup_requested_.load() ||
                phone_epoch != phone_setup_epoch_.load()) {
                vibe_phone_setup::stop();
                phone_setup_started = false;
                if (!active_.load() && vibe_wifi::receiver_active()) (void)vibe_wifi::restore_station();
                continue;
            }
            vibe_phone_setup::tick();
            const auto status = vibe_phone_setup::snapshot();
            if (status.phase == vibe_phone_setup::Phase::Expired || status.phase == vibe_phone_setup::Phase::Error) {
                vibe_phone_setup::stop();
                phone_setup_started = false;
                fail_phone(status.phase == vibe_phone_setup::Phase::Expired ? T("手机配置已超时，请重新开启") : T("手机配置开启失败，请返回重试"));
                continue;
            }
            vibe_phone_setup::Submission submission;
            if (vibe_phone_setup::take_submission(submission)) {
                vibe_phone_setup::stop();
                phone_setup_started = false;
                if (active_.load() && !exit_requested_.load() && phone_setup_requested_.load() &&
                    phone_epoch == phone_setup_epoch_.load()) {
                    std::string error;
                    bool saved = false;
                    if (vibe_wifi::setup_ap_active()) error = T("手机配置关闭失败，请返回重试");
                    else saved = applyPhoneSetup(submission, error);
                    if (saved) {
                        if (phone_epoch == phone_setup_epoch_.load() && phone_setup_requested_.load()) {
                            phone_setup_requested_.store(false);
                            phone_setup_result_.store(1);
                        }
                    } else fail_phone(error.empty() ? T("手机配置保存失败，请重试") : vibe_i18n::message(error));
                }
                std::fill(submission.wifi_password.begin(), submission.wifi_password.end(), '\0');
                std::fill(submission.receiver_password.begin(), submission.receiver_password.end(), '\0');
                last_heartbeat_ms = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (locked_.load() || (!foreground && !has_pending_action)) {
            last_heartbeat_ms = 0; // Re-entering Vibe sends the first heartbeat immediately.
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // Apply saved mode changes even when the previous Wi-Fi is offline.
        if (foreground) vibe_pairing::apply_pending_access();
        // Entry/exit station switching shares this worker with phone setup.
        // A rapid reopen cannot race an old exit into the wrong Wi-Fi mode.
        if (canOperate() && foreground_network_pending_.exchange(false)) {
            if (vibe_pairing::snapshot().access_mode == vibe_pairing::AccessMode::Receiver &&
                !vibe_wifi::receiver_active() && vibe_wifi::connect_receiver() != ESP_OK)
                setConnectionError(T("请在接入设置填写接收端热点信息"));
        }
        const bool usb_direct_active = canOperate() &&
            vibe_pairing::snapshot().access_mode == vibe_pairing::AccessMode::UsbDirect;
        if (vibe_usb::active() != usb_direct_active) vibe_usb::set_active(usb_direct_active);
        if (!canOperate()) vibe_usb::set_active(false);
        // Both USB transports handle one request exchange at a time.
        // Transcription can take minutes; let the voice task own the link.
        if (foreground && receiverVoiceUploading()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        auto failPendingAction = [this](const char *reason) {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (!pending_action_.verb.empty()) {
                pending_action_ = {};
                model_.action_in_flight = false;
                model_.feedback_external = false;
                model_.feedback = reason;
                ++model_.revision;
            }
            if (pending_create_session_) {
                pending_create_session_ = false;
                pending_create_provider_id_.clear();
                pending_create_project_id_.clear();
                model_.session_notice = reason;
                ++model_.revision;
            }
        };
        const bool usb_direct = vibe_pairing::snapshot().access_mode == vibe_pairing::AccessMode::UsbDirect;
        wifi_ap_record_t ap = {};
        if (!usb_direct && (!vibe_wifi::is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK)) {
            const bool receiver = vibe_pairing::snapshot().access_mode == vibe_pairing::AccessMode::Receiver ||
                vibe_wifi::receiver_active();
            if (receiver && vibe_pairing::snapshot().phase == vibe_pairing::Phase::Paired)
                vibe_pairing::begin_foreground_connection();
            invalidateCatalog();
            setConnectionError(receiver ? "Connect receiver hotspot first" : "Connect Wi-Fi first");
            failPendingAction(receiver ? "Action not sent: receiver offline" :
                              "Action not sent: Wi-Fi offline");
            vTaskDelay(pdMS_TO_TICKS(foreground ? POLL_INTERVAL_MS : 500));
            continue;
        }
        if (foreground) vibe_pairing::tick();
        const auto access = vibe_pairing::snapshot();
        if (access.phase == vibe_pairing::Phase::Paired &&
            access.url.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) {
            setConnectionError(vibe_pairing::tls_time_failed() ?
                T("设备时间同步失败，无法校验 HTTPS 证书") : T("正在同步设备时间，以校验 HTTPS 证书"));
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }
        const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        if (canOperate() && vibe_pairing::authorized_for_foreground() &&
            vibe_pairing::snapshot().phase == vibe_pairing::Phase::Paired &&
            (last_heartbeat_ms == 0 || now_ms - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS)) {
            last_heartbeat_ms = now_ms;
            sendHeartbeat();
        }
        const auto pairing = vibe_pairing::snapshot();
        if (pairing.phase != vibe_pairing::Phase::Paired ||
            !vibe_pairing::authorized_for_foreground()) {
            setConnectionError(pairing.message);
            failPendingAction("Action not sent: computer not paired");
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }
        if (locked_.load()) continue;
        if (!active_.load()) {
            // Finish an already requested confirmation/cancellation, then go idle.
            PendingAction action;
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                action = std::move(pending_action_);
                pending_action_ = {};
            }
            if (!action.verb.empty()) sendAction(std::move(action));
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (canOperate() &&
            (language_refresh_pending_.exchange(false) || (active_cycles++ % 14) == 0 || !config_loaded_.load() || !voice_available_.load())) {
            refreshConfig();
        }
        PendingAction action;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            action = std::move(pending_action_);
            pending_action_ = {};
        }
        if (!action.verb.empty()) sendAction(std::move(action));
        std::string create_provider;
        std::string create_project;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (pending_create_session_ && !create_session_in_flight_) {
                create_provider = std::move(pending_create_provider_id_);
                create_project = std::move(pending_create_project_id_);
                pending_create_session_ = false;
                pending_create_provider_id_.clear();
                create_session_in_flight_ = true;
            }
        }
        if (!create_provider.empty()) createSession(create_provider, create_project);
        std::string delete_provider;
        std::string delete_session_id;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (pending_delete_session_ && !delete_session_in_flight_) {
                delete_provider = std::move(pending_delete_provider_id_);
                delete_session_id = std::move(pending_delete_session_id_);
                pending_delete_session_ = false;
                pending_delete_provider_id_.clear();
                pending_delete_session_id_.clear();
                delete_session_in_flight_ = true;
            }
        }
        if (!delete_session_id.empty()) deleteSession(delete_provider, delete_session_id);
        if (canOperate() && config_loaded_.load()) {
            refreshSessions();
            refreshTasks();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

} // namespace esp_brookesia::apps
