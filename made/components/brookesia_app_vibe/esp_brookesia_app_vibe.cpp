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

// debug.heap 命令：dump 堆诊断（debug.heap）。需要 CONFIG_HEAP_TASK_TRACKING=y
// 才有 per-task 表——用它把「heap 越用越少」点名到具体线程（2026-10-19 晨
// 实机：解锁后 free 仅 303KB / largest 156KB，与前一晚差 ~800KB，来源不明）。
void logHeapDiagnostics()
{
    ESP_LOGW("vibe_heap",
        "[heap] psram free=%u largest=%u min=%u | internal free=%u largest=%u min=%u",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    // 注意：CONFIG_HEAP_TASK_TRACKING=y 在 IDF v5.5.5 上开机即崩
    // （heap_caps_update_per_task_info_alloc 早期中断分配空指针，
    // 实机 2026-10-19 复现），任务级表保持关闭，这里只出池级统计。
    const auto pool = [](const char *name, uint32_t caps) {
        ESP_LOGW("vibe_heap", "[heap][%s] free=%u largest=%u min=%u", name,
            static_cast<unsigned>(heap_caps_get_free_size(caps)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(caps)),
            static_cast<unsigned>(heap_caps_get_minimum_free_size(caps)));
    };
    pool("psram", MALLOC_CAP_SPIRAM);
    pool("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    pool("dma", MALLOC_CAP_DMA);
    pool("8bit", MALLOC_CAP_8BIT);
#if CONFIG_HEAP_TASK_TRACKING
    constexpr int kMaxTasks = 24;
    static heap_task_totals_t s_totals[kMaxTasks];
    static size_t s_num_totals = 0;
    heap_task_info_params_t params = {};
    params.caps[0] = MALLOC_CAP_INTERNAL;
    params.mask[0] = MALLOC_CAP_INTERNAL;
    params.caps[1] = MALLOC_CAP_SPIRAM;
    params.mask[1] = MALLOC_CAP_SPIRAM;
    params.tasks = nullptr;          // nullptr = 全部任务
    params.num_tasks = 0;
    params.totals = s_totals;
    params.num_totals = &s_num_totals;
    params.max_totals = kMaxTasks;
    params.blocks = nullptr;         // 只要任务汇总，不要逐块明细
    params.max_blocks = 0;
    heap_caps_get_per_task_info(&params);
    for (size_t i = 0; i < s_num_totals; ++i) {
        const char *name = s_totals[i].task ? pcTaskGetName(s_totals[i].task) : "(deleted)";
        ESP_LOGW("vibe_heap", "[heap][task] %-16s internal=%uKB blocks=%u spiram=%uKB blocks=%u",
            name ? name : "?",
            static_cast<unsigned>(s_totals[i].size[0] / 1024),
            static_cast<unsigned>(s_totals[i].count[0]),
            static_cast<unsigned>(s_totals[i].size[1] / 1024),
            static_cast<unsigned>(s_totals[i].count[1]));
    }
#endif
}

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
    lv_obj_t *screen = lv_scr_act();
    // Brookesia's default screen is not clickable. Restore hit testing so
    // swipes beginning on blank space or a non-clickable label reach Vibe.
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    // 真翻页：允许子对象渲染到屏幕边界之外。侧舞台停在屏外 ±1 屏宽，
    // 拖动平移时从屏边滑入视野。两个条件缺一不可：
    //   LV_OBJ_FLAG_OVERFLOW_VISIBLE——子对象裁剪从「屏幕自身坐标」放宽到
    //   「坐标+ext_draw_size」；ext_draw_size——放宽量（0 的话等于没放宽），
    //   同时也让屏幕平移时的失效区域盖住滑入的侧舞台。
    lv_obj_add_flag(screen, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_ext_draw_size(screen, made_screen_w());
    // set_ext_draw_size 只注册回调，必须显式刷新一次才真正写入 spec_attr。
    lv_obj_refresh_ext_draw_size(screen);
    // 兜底：任何异常路径残留的横向偏移在重建界面时归位，避免整屏卡在边上。
    if (!home_dragging_) lv_obj_set_x(screen, 0);
    // 助手（精灵）舞台背景独立成 token（home_bg），主题包可单独给主页配色，
    // 不跟抽屉/设置面板的 bg_page 混用。
    lv_obj_set_style_bg_color(screen, lv_color_hex(VT(home_bg)), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    // 拖动翻屏时移出屏幕边缘露出的 bottom layer：LVGL 默认是白色，
    // 改成助手背景色，滑动过渡才不会闪白层。
    lv_obj_t *bottom_layer = lv_display_get_layer_bottom(lv_display_get_default());
    if (bottom_layer) {
        lv_obj_set_style_bg_color(bottom_layer, lv_color_hex(VT(home_bg)), 0);
        lv_obj_set_style_bg_opa(bottom_layer, LV_OPA_COVER, 0);
    }
    lv_obj_add_event_cb(screen, providerSwipeCallback, LV_EVENT_GESTURE, this);
    // 主页上滑拉出抽屉；任务切换手势收进抽屉内部处理。
    lv_obj_add_event_cb(screen, drawerSwipeCallback, LV_EVENT_GESTURE, this);
    // 翻屏跟手：按下记录起点，拖动平移首页，松手按阈值吸附/切助手。
    lv_obj_add_event_cb(screen, homeDragCallback, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(screen, homeDragCallback, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(screen, homeDragCallback, LV_EVENT_RELEASED, this);
    // 助手页双击（屏幕空白处连点两下）：插件可订阅 touch.double 做动作触发。
    lv_obj_add_event_cb(screen, homeShortClickCallback, LV_EVENT_SHORT_CLICKED, this);

    // 近全屏小精灵：主页的绝对主角（docs/SPRITES.md 的素材机制接入前用程序化形象）。
    sprite_stage_ = made_sprite::create(screen);
    // 左右侧台：常驻解码好左右相邻助手（实测三套合计 ~2.2MB），拖动时随屏
    // 滑入视野；松手晋升是指针轮换，素材永不重解码。侧台关烘焙：烘焙是
    // 内存换 CPU（原始帧被释放、换成更大的多帧位图），侧台动画走动态缩放。
    for (auto &slot : sides_) {
        slot.stage = made_sprite::create(screen);
        made_sprite::setBakeEnabled(slot.stage, false);
    }
    sides_[0].side = -1;
    sides_[1].side = 1;
    lv_obj_set_x(sides_[0].stage, -made_screen_w());     // 屏外左侧
    lv_obj_set_x(sides_[1].stage, made_screen_w());      // 屏外右侧
    sprite_started_ms_ = lv_tick_get();

    // Artwork arrives with the authorized bridge catalog. Only the selected
    // image is expanded; the other entries retain their bounded packed pixels.
    // 顶部的助手头像徽章已撤掉：精灵素材本身就是助手本体。
    rendered_catalog_ready_ = false;

    page_label_ = lv_label_create(screen);
    // 助手页底部：名字之下的页码（左右滑切精灵的指示）。
    lv_obj_set_pos(page_label_, made_x(150), made_home_y(338));
    lv_obj_set_width(page_label_, made_s(60));
    lv_obj_set_style_text_align(page_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(page_label_, lv_color_hex(VT(text_hint)), 0);
    lv_obj_set_style_text_font(page_label_, &lv_font_montserrat_12, 0);

    // Creating a conversation is explicit. This touch target is large enough
    // for the small round screen and never doubles as the record button.
    new_session_button_ = lv_obj_create(screen);
    // 加号与齿轮镜像：齿轮 40..86，加号 274..320，离各自屏幕边缘同为 40。
    // 圆形按钮（r23）在 y56..102 一行贴到这个位置仍完整落在圆屏弦内。
    lv_obj_set_pos(new_session_button_, made_x(274), made_y(56));
    lv_obj_set_size(new_session_button_, made_s(46), made_s(46));
    lv_obj_set_style_radius(new_session_button_, 23, 0);
    lv_obj_set_overflow_visible(new_session_button_, true);
    // 抽屉动作栏主次分层：+ 是主操作（accent 实底），− 是破坏性操作（弱化底）。
    styleSurface(new_session_button_, VT(accent));
    lv_obj_set_style_pad_all(new_session_button_, 0, 0);
    lv_obj_remove_flag(new_session_button_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(new_session_button_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(new_session_button_, trackTap, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(new_session_button_, ignoreSwipeClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(new_session_button_, newSessionCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *new_session_mark = lv_label_create(new_session_button_);
    lv_label_set_text(new_session_mark, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(new_session_mark, lv_color_hex(VT(text_on_accent)), 0);
    lv_obj_set_style_text_font(new_session_mark, &lv_font_montserrat_26, 0);
    lv_obj_remove_flag(new_session_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(new_session_mark);

    delete_session_button_ = lv_obj_create(screen);
    // 减号放头像（右缘 217）和加号（左缘 274）正中间：222..268，不遮动物。
    lv_obj_set_pos(delete_session_button_, made_x(222), made_y(56));
    lv_obj_set_size(delete_session_button_, made_s(46), made_s(46));
    lv_obj_set_style_radius(delete_session_button_, 23, 0);
    lv_obj_set_overflow_visible(delete_session_button_, true);
    styleSurface(delete_session_button_, VT(danger_soft));
    lv_obj_set_style_pad_all(delete_session_button_, 0, 0);
    lv_obj_remove_flag(delete_session_button_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(delete_session_button_, trackTap, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(delete_session_button_, ignoreSwipeClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(delete_session_button_, deleteSessionCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *delete_session_mark = lv_label_create(delete_session_button_);
    lv_label_set_text(delete_session_mark, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(delete_session_mark, lv_color_hex(VT(text_secondary)), 0);
    lv_obj_set_style_text_font(delete_session_mark, &lv_font_montserrat_26, 0);
    lv_obj_remove_flag(delete_session_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(delete_session_mark);
    lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(new_session_button_);

    // The access editor is a modal surface. Its touch keyboard fits within
    // the round screen, and BOOT short clicks are ignored while it is open.
    settings_button_ = touchButton(screen, 40, 56, 46, 46, nullptr, settingsCallback, this);
    lv_obj_set_style_radius(settings_button_, 23, 0);
    lv_obj_set_ext_click_area(settings_button_, 10);
    lv_obj_t *gear_icon = lv_label_create(settings_button_);
    lv_label_set_text(gear_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear_icon, lv_color_hex(VT(text_primary)), 0);
    lv_obj_set_style_text_font(gear_icon, &lv_font_montserrat_26, 0);
    lv_obj_remove_flag(gear_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(gear_icon);

    settings_overlay_ = lv_obj_create(screen);
    lv_obj_set_pos(settings_overlay_, made_s(0), made_s(0));
    lv_obj_set_size(settings_overlay_, made_screen_w(), made_screen_h());
    lv_obj_set_style_bg_color(settings_overlay_, lv_color_hex(VT(bg_page)), 0);
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
    // 设置主页：分组行列表（连接/偏好/系统），行内 图标+名称+箭头；
    // 返回改顶部导航，释放底部空间给列表滚动。
    // 设置页标题与正文同为 16px，靠留白和返回键区分层级。
    lv_obj_t *settings_title = lv_label_create(settings_home_);
    lv_label_set_text(settings_title, T("设置"));
    bindLabel(settings_title, "设置");
    lv_obj_set_style_text_color(settings_title, lv_color_hex(VT(title)), 0);
    lv_obj_set_style_text_font(settings_title, text_font, 0);
    lv_obj_set_pos(settings_title, made_page_x(78), made_page_y(32));
    lv_obj_set_style_text_color(settings_title, lv_color_hex(VT(title)), 0);
    lv_obj_t *settings_back = touchButton(settings_home_, 16, 30, 48, 36, nullptr, settingsCloseCallback, this);
    lv_obj_t *settings_back_icon = lv_label_create(settings_back);
    lv_label_set_text(settings_back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(settings_back_icon, lv_color_hex(VT(text_secondary)), 0);
    lv_obj_set_style_text_font(settings_back_icon, &lv_font_montserrat_16, 0);
    lv_obj_center(settings_back_icon);
    lv_obj_remove_flag(settings_back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *settings_rows = lv_obj_create(settings_home_);
    // 行宽与任务页回显卡片同几何（x12 宽 336）：行右缘离屏边 12 设计像素，
    // chevron 与分隔线随之贴右，右侧不再留一整条空白带（2026-10-17 真机反馈）。
    lv_obj_set_pos(settings_rows, made_page_x(12), made_page_y(76));
    lv_obj_set_size(settings_rows, made_page_w(336), made_page_y(348) - made_page_y(76));
    lv_obj_set_style_bg_opa(settings_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_rows, 0, 0);
    lv_obj_set_style_pad_all(settings_rows, 0, 0);
    lv_obj_set_scroll_dir(settings_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_rows, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(settings_rows, LV_OBJ_FLAG_GESTURE_BUBBLE);
    auto settings_section = [&](int y, const char *key) {
        lv_obj_t *label = lv_label_create(settings_rows);
        lv_label_set_text(label, T(key));
        bindLabel(label, key);
        lv_obj_set_style_text_color(label, lv_color_hex(VT(text_hint)), 0);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_pos(label, made_s(4), made_s(y));
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    };
    auto settings_row = [&](int y, const char *symbol, const char *key, lv_event_cb_t cb) {
        lv_obj_t *row = touchButton(settings_rows, 0, 0, 336, 46, nullptr, cb, this);
        lv_obj_set_pos(row, 0, made_s(y));
        // Air 语言：行透明无边框，按下整行泛灰；底部 1px 分隔线从文字左缘
        // 开始（iOS 式），不再用实底胶囊行——界面从「色块堆叠」变「线条留白」。
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(VT(bg_row)), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(VT(border)), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        // 裸图标：直接用 tint 色线性图标，去掉 icon_box 色块。
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_color(icon, lv_color_hex(VT(text_accent)), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, made_s(10), 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, T(key));
        bindLabel(name, key);
        lv_obj_set_style_text_color(name, lv_color_hex(VT(text_primary)), 0);
        lv_obj_set_style_text_font(name, text_font, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, made_s(42), 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *chevron = lv_label_create(row);
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chevron, lv_color_hex(VT(text_hint)), 0);
        lv_obj_set_style_text_font(chevron, &lv_font_montserrat_14, 0);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -made_s(12), 0);
        lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
    };
    settings_section(6, "settings.connection");
    settings_row(30, LV_SYMBOL_USB, "settings.access", accessOpenCallback);
    settings_row(78, LV_SYMBOL_WIFI, T("网络设置"), wifiScanOpenCallback);
    settings_section(132, "settings.prefs");
    settings_row(154, LV_SYMBOL_TINT, T("主题设置"), themeOpenCallback);
    settings_row(202, LV_SYMBOL_VOLUME_MAX, "settings.volume", volumeOpenCallback);
    settings_row(250, LV_SYMBOL_LIST, "settings.language", languageOpenCallback);
    settings_section(304, "settings.system");
    settings_row(326, LV_SYMBOL_POWER, T("电源设置"), powerOpenCallback);
    settings_row(374, LV_SYMBOL_DOWNLOAD, T("固件更新"), firmwareOpenCallback);

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
    language_hint_ = settingsLabel(language_panel_, T("设备语言独立保存"), 55, 259, 250, VT(text_hint));
    touchButton(language_panel_, 125, 292, 110, 34, T("返回"), languageBackCallback, this);
    lv_obj_add_flag(language_panel_, LV_OBJ_FLAG_HIDDEN);

    theme_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(theme_panel_, made_screen_w(), made_screen_h());
    markPage(theme_panel_);
    lv_obj_set_style_bg_opa(theme_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(theme_panel_, 0, 0);
    lv_obj_set_style_pad_all(theme_panel_, 0, 0);
    lv_obj_remove_flag(theme_panel_, LV_OBJ_FLAG_SCROLLABLE);
    settingsLabel(theme_panel_, T("主题设置"), 70, 34, 220);
    touchButton(theme_panel_, 70, 60, 220, 32, T("从电脑同步主题"), themeSyncCallback, this);
    theme_status_label_ = settingsLabel(theme_panel_, "", 40, 96, 280, VT(text_hint));
    theme_rows_ = lv_obj_create(theme_panel_);
    lv_obj_set_pos(theme_rows_, made_page_x(40), made_page_y(112));
    lv_obj_set_size(theme_rows_, made_page_w(280), made_page_y(280) - made_page_y(112));
    lv_obj_set_style_bg_opa(theme_rows_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(theme_rows_, 0, 0);
    lv_obj_set_style_pad_all(theme_rows_, 0, 0);
    lv_obj_set_scroll_dir(theme_rows_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(theme_rows_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(theme_rows_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    touchButton(theme_panel_, 125, 292, 110, 34, T("返回"), themeBackCallback, this);
    lv_obj_add_flag(theme_panel_, LV_OBJ_FLAG_HIDDEN);

    power_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(power_panel_, made_screen_w(), made_screen_h());
    markPage(power_panel_);
    lv_obj_set_style_bg_opa(power_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(power_panel_, 0, 0);
    lv_obj_set_style_pad_all(power_panel_, 0, 0);
    lv_obj_remove_flag(power_panel_, LV_OBJ_FLAG_SCROLLABLE);
    settingsLabel(power_panel_, T("电源设置"), 70, 38, 220);
    settingsLabel(power_panel_, T("空闲自动熄屏"), 55, 100, 250, VT(text_secondary));
    power_timeout_button_ = touchButton(power_panel_, 70, 130, 220, 46, nullptr, powerTimeoutCallback, this);
    power_timeout_label_ = settingsLabel(power_timeout_button_, "", 0, 12, 220);
    settingsLabel(power_panel_, T("关机后点一下屏幕开机。熄屏只关背光，触摸即可点亮"), 45, 188, 270, VT(text_hint));
    lv_obj_t *shutdown = touchButton(power_panel_, 70, 236, 220, 46, nullptr, powerShutdownCallback, this);
    power_shutdown_label_ = settingsLabel(shutdown, T("关机"), 0, 12, 220);
    touchButton(power_panel_, 125, 292, 110, 34, T("返回"), powerBackCallback, this);
    lv_obj_add_flag(power_panel_, LV_OBJ_FLAG_HIDDEN);
    loadScreenOffTimeout();

    firmware_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(firmware_panel_, made_screen_w(), made_screen_h());
    markPage(firmware_panel_);
    lv_obj_set_style_bg_opa(firmware_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(firmware_panel_, 0, 0);
    lv_obj_set_style_pad_all(firmware_panel_, 0, 0);
    lv_obj_remove_flag(firmware_panel_, LV_OBJ_FLAG_SCROLLABLE);
    settingsLabel(firmware_panel_, T("固件更新"), 70, 38, 220);
    firmware_status_label_ = settingsLabel(firmware_panel_, T("电脑上还没有配置固件地址"), 45, 100, 270, VT(text_secondary), false);
    lv_label_set_long_mode(firmware_status_label_, LV_LABEL_LONG_WRAP);
    settingsLabel(firmware_panel_, T("只有在这里点下载才会更新"), 45, 168, 270, VT(text_hint));
    touchButton(firmware_panel_, 70, 220, 220, 46, T("下载更新"), firmwareUpdateCallback, this);
    touchButton(firmware_panel_, 125, 292, 110, 34, T("返回"), firmwareBackCallback, this);
    lv_obj_add_flag(firmware_panel_, LV_OBJ_FLAG_HIDDEN);

    volume_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(volume_panel_, made_screen_w(), made_screen_h());
    markPage(volume_panel_);
    lv_obj_set_style_bg_opa(volume_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(volume_panel_, 0, 0);
    lv_obj_set_style_pad_all(volume_panel_, 0, 0);
    lv_obj_remove_flag(volume_panel_, LV_OBJ_FLAG_SCROLLABLE);
    settingsLabel(volume_panel_, "settings.volume", 70, 48, 220);
    volume_value_label_ = settingsLabel(volume_panel_, "", 70, 118, 220, 0, false);
    settingsLabel(volume_panel_, "提示音和语音回答都用这个音量", 45, 168, 270, VT(text_hint));
    touchButton(volume_panel_, 60, 214, 110, 46, "减小", volumeDownCallback, this);
    touchButton(volume_panel_, 190, 214, 110, 46, "增大", volumeUpCallback, this);
    touchButton(volume_panel_, 125, 292, 110, 34, T("返回"), volumeBackCallback, this);
    lv_obj_add_flag(volume_panel_, LV_OBJ_FLAG_HIDDEN);
    renderVolume();

    access_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(access_panel_, made_screen_w(), made_screen_h());
    markPage(access_panel_);
    lv_obj_set_style_bg_opa(access_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(access_panel_, 0, 0);
    lv_obj_set_style_pad_all(access_panel_, 0, 0);
    lv_obj_remove_flag(access_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *access_back = touchButton(access_panel_, 48, 292, 120, 36, T("返回"), accessBackCallback, this, VT(btn_cancel));
    settingsLabel(access_panel_, T("接入设置"), 70, 36, 220);
    access_save_button_ = touchButton(access_panel_, 192, 292, 120, 36, T("保存"), accessSaveCallback, this, VT(accent));
    (void)access_back;
    // Choose a transport separately from editing its fields. Four narrow tabs
    // were easy to mistap on this round display; these targets have 18 px gaps.
    access_picker_hint_ = settingsLabel(access_panel_, T("选择连接方式"), 65, 78, 230, VT(text_secondary));
    access_auto_button_ = touchButton(access_panel_, 45, 104, 126, 64,
                                      "Wi-Fi", accessModeCallback, this);
    access_manual_button_ = touchButton(access_panel_, 189, 104, 126, 64,
                                        T("手动地址"), accessModeCallback, this);
    access_receiver_button_ = touchButton(access_panel_, 45, 186, 126, 64,
                                          T("接收端"), accessModeCallback, this);
    access_usb_button_ = touchButton(access_panel_, 189, 186, 126, 64,
                                     T("USB 直连"), accessModeCallback, this);
    phone_setup_button_ = touchButton(access_panel_, 70, 256, 220, 30,
                                       "手机配置", phoneSetupOpenCallback, this, VT(accent));
    access_change_button_ = touchButton(access_panel_, 54, 74, 252, 32,
                                        nullptr, accessPickerCallback, this);
    access_change_label_ = settingsLabel(access_change_button_, "", 0, 6, 252);
    lv_obj_set_pos(access_change_label_, made_s(0), made_s(6));
    lv_obj_remove_flag(access_change_label_, LV_OBJ_FLAG_CLICKABLE);
    access_status_label_ = settingsLabel(access_panel_, T("公网地址请选择 HTTPS"), 47, 109, 266, VT(text_accent));
    lv_label_set_long_mode(access_status_label_, LV_LABEL_LONG_DOT);
    access_usb_hint_ = settingsLabel(access_panel_,
        T("用数据线连接电脑\n打开电脑桥接器\n保存后核对六位配对码"), 65, 152, 230, VT(text_secondary));
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
    lv_obj_set_style_bg_color(receiver_picker_, lv_color_hex(VT(bg_page)), 0);
    lv_obj_set_style_bg_opa(receiver_picker_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(receiver_picker_, 0, 0);
    lv_obj_set_style_pad_all(receiver_picker_, 0, 0);
    lv_obj_remove_flag(receiver_picker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(receiver_picker_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    settingsLabel(receiver_picker_, "选择接收端", 65, 36, 230);
    receiver_list_ = lv_obj_create(receiver_picker_);
    lv_obj_set_pos(receiver_list_, made_page_x(45), made_page_y(68));
    lv_obj_set_size(receiver_list_, made_page_w(270), made_page_y(236) - made_page_y(68));
    lv_obj_set_style_bg_opa(receiver_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(receiver_list_, 0, 0);
    lv_obj_set_style_pad_all(receiver_list_, 0, 0);
    lv_obj_set_scroll_dir(receiver_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(receiver_list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(receiver_list_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    receiver_hint_ = settingsLabel(receiver_picker_, "", 60, 242, 240, VT(text_secondary));
    touchButton(receiver_picker_, 48, 292, 88, 36, "返回", receiverScanBackCallback, this);
    touchButton(receiver_picker_, 136, 292, 88, 36, "手动填写", receiverScanBackCallback, this);
    receiver_refresh_button_ = touchButton(receiver_picker_, 224, 292, 88, 36,
        "刷新", receiverScanRefreshCallback, this);
    lv_obj_add_flag(receiver_picker_, LV_OBJ_FLAG_HIDDEN);
    receiver_scan_revision_ = UINT32_MAX;
    // 选网是独立的网络设置，不挂在连接电脑的方式下面。
    wifi_picker_ = lv_obj_create(settings_overlay_);
    lv_obj_set_pos(wifi_picker_, made_s(0), made_s(0));
    lv_obj_set_size(wifi_picker_, made_screen_w(), made_screen_h());
    markPage(wifi_picker_);
    lv_obj_set_style_bg_color(wifi_picker_, lv_color_hex(VT(bg_page)), 0);
    lv_obj_set_style_bg_opa(wifi_picker_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_picker_, 0, 0);
    lv_obj_set_style_pad_all(wifi_picker_, 0, 0);
    lv_obj_remove_flag(wifi_picker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wifi_picker_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    settingsLabel(wifi_picker_, T("网络设置"), 65, 36, 230);
    wifi_network_list_ = lv_obj_create(wifi_picker_);
    lv_obj_set_pos(wifi_network_list_, made_page_x(45), made_page_y(96));
    lv_obj_set_size(wifi_network_list_, made_page_w(270), made_page_y(268) - made_page_y(96));
    lv_obj_set_style_bg_opa(wifi_network_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_network_list_, 0, 0);
    lv_obj_set_style_pad_all(wifi_network_list_, 0, 0);
    lv_obj_set_scroll_dir(wifi_network_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wifi_network_list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(wifi_network_list_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    wifi_scan_status_ = settingsLabel(wifi_picker_, "", 50, 64, 260, VT(text_secondary));
    touchButton(wifi_picker_, 48, 292, 120, 36, "返回", wifiScanBackCallback, this);
    touchButton(wifi_picker_, 192, 292, 120, 36, "刷新", wifiScanRefreshCallback, this);
    lv_obj_add_flag(wifi_picker_, LV_OBJ_FLAG_HIDDEN);
    wifi_scan_list_revision_ = UINT32_MAX;
    // A field opens a full-screen editor with five wide keys per row.
    access_editor_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_pos(access_editor_panel_, made_s(0), made_s(0));
    lv_obj_set_size(access_editor_panel_, made_screen_w(), made_screen_h());
    markPage(access_editor_panel_);
    lv_obj_set_style_bg_color(access_editor_panel_, lv_color_hex(VT(bg_panel)), 0);
    lv_obj_set_style_bg_opa(access_editor_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(access_editor_panel_, 0, 0);
    lv_obj_set_style_pad_all(access_editor_panel_, 0, 0);
    lv_obj_remove_flag(access_editor_panel_, LV_OBJ_FLAG_SCROLLABLE);
    touchButton(access_editor_panel_, 78, 29, 76, 38, T("取消"), accessEditorCancelCallback, this, VT(btn_cancel));
    touchButton(access_editor_panel_, 206, 29, 76, 38, T("完成"), accessEditorDoneCallback, this, VT(accent));
    access_editor_title_ = settingsLabel(access_editor_panel_, "", 55, 75, 250, VT(text_secondary));
    access_editor_input_ = lv_textarea_create(access_editor_panel_);
    lv_obj_set_pos(access_editor_input_, made_page_x(54), made_page_y(105));
    lv_obj_set_size(access_editor_input_, made_page_w(252), made_page_h(44));
    lv_textarea_set_one_line(access_editor_input_, true);
    lv_obj_set_style_text_font(access_editor_input_, text_font, LV_PART_MAIN);
    lv_obj_add_event_cb(access_editor_input_, accessEditorDoneCallback, LV_EVENT_READY, this);
    for (lv_obj_t *field : {access_host_input_, access_port_input_,
                           access_password_input_, access_editor_input_}) {
        lv_obj_set_style_bg_color(field, lv_color_hex(VT(bg_row)), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(field, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(field, lv_color_hex(VT(text_primary)), LV_PART_MAIN);
        lv_obj_set_style_text_color(field, lv_color_hex(VT(text_hint)), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_border_color(field, lv_color_hex(VT(border)), LV_PART_MAIN);
        lv_obj_set_style_border_width(field, 1, LV_PART_MAIN);
    }
    lv_obj_add_flag(access_editor_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(access_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_overlay_, LV_OBJ_FLAG_HIDDEN);

    phone_setup_panel_ = lv_obj_create(settings_overlay_);
    lv_obj_set_size(phone_setup_panel_, made_screen_w(), made_screen_h());
    markPage(phone_setup_panel_);
    lv_obj_set_style_bg_color(phone_setup_panel_, lv_color_hex(VT(bg_page)), 0);
    lv_obj_set_style_bg_opa(phone_setup_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(phone_setup_panel_, 0, 0);
    lv_obj_set_style_pad_all(phone_setup_panel_, 0, 0);
    lv_obj_remove_flag(phone_setup_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(phone_setup_panel_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *phone_back = touchButton(phone_setup_panel_, 125, 292, 110, 36, "返回", phoneSetupBackCallback, this);
    lv_obj_t *phone_title = settingsLabel(phone_setup_panel_, "手机配置", 70, 32, 220);
    (void)phone_back;
    phone_setup_hint_ = settingsLabel(phone_setup_panel_, "", 55, 69, 250, VT(text_accent), false);
    phone_setup_qr_ = lv_qrcode_create(phone_setup_panel_);
    lv_qrcode_set_size(phone_setup_qr_, made_s(120));
    lv_qrcode_set_dark_color(phone_setup_qr_, lv_color_black());
    lv_qrcode_set_light_color(phone_setup_qr_, lv_color_white());
    lv_qrcode_set_quiet_zone(phone_setup_qr_, true);
    lv_obj_set_pos(phone_setup_qr_, (made_screen_w() - made_s(120)) / 2, made_page_y(78));
    lv_obj_add_flag(phone_setup_qr_, LV_OBJ_FLAG_HIDDEN);
    phone_setup_network_ = settingsLabel(phone_setup_panel_, "", 55, 206, 250, VT(title), false);
    phone_setup_password_ = settingsLabel(phone_setup_panel_, "", 65, 226, 230, VT(title), false);
    phone_setup_address_ = settingsLabel(phone_setup_panel_, "", 85, 246, 190, VT(text_secondary), false);
    lv_obj_add_flag(phone_setup_panel_, LV_OBJ_FLAG_HIDDEN);

    provider_label_ = lv_label_create(screen);
    lv_obj_set_pos(provider_label_, made_x(70), made_home_y(302));
    lv_obj_set_style_text_align(provider_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(provider_label_, lv_color_hex(VT(text_primary)), 0);
    lv_obj_set_style_text_font(provider_label_, &lv_font_montserrat_26, 0);
    made_text(provider_label_, 220, false);

    // 素材页顶部状态图标（单一位置换图标，设计稿 2026-10-19）：药丸底
    // bg_row + 48×48 recolor 位图（2026-10-20 放大 3 倍）。抽屉打开时隐藏
    // （抽屉右上已有文字徽章）。
    const int icon_pill_w = made_s(76);
    const int icon_pill_h = made_s(60);
    status_icon_pill_ = lv_obj_create(screen);
    lv_obj_remove_style_all(status_icon_pill_);
    lv_obj_set_size(status_icon_pill_, icon_pill_w, icon_pill_h);
    lv_obj_set_pos(status_icon_pill_, made_x(180) - icon_pill_w / 2, made_home_y(6));
    lv_obj_set_style_radius(status_icon_pill_, icon_pill_h / 2, 0);
    lv_obj_set_style_bg_color(status_icon_pill_, lv_color_hex(VT(bg_row)), 0);
    lv_obj_set_style_bg_opa(status_icon_pill_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(status_icon_pill_, LV_OBJ_FLAG_CLICKABLE);
    status_icon_img_ = lv_image_create(status_icon_pill_);
    lv_image_set_src(status_icon_img_, &kStatusIconIdle);
    lv_image_set_scale(status_icon_img_, made_lv_scale());
    lv_obj_center(status_icon_img_);
    lv_obj_set_style_img_recolor(status_icon_img_, lv_color_hex(VT(text_hint)), 0);
    lv_obj_set_style_img_recolor_opa(status_icon_img_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(status_icon_img_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(status_icon_pill_, LV_OBJ_FLAG_HIDDEN);

    // 顶部状态角标（主页专属，不进抽屉）：左上圆点 = 助手连接/工作状态，
    // 右上铃铛 = 有新回显还没打开抽屉看过。不拦截触摸与手势。
    status_dot_ = lv_obj_create(screen);
    lv_obj_remove_style_all(status_dot_);
    lv_obj_set_size(status_dot_, made_s(12), made_s(12));
    lv_obj_set_pos(status_dot_, made_x(22), made_y(14));
    lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_dot_, lv_color_hex(VT(success)), 0);
    lv_obj_set_style_border_width(status_dot_, 2, 0);
    lv_obj_set_style_border_color(status_dot_, lv_color_hex(VT(bg_page)), 0);
    lv_obj_clear_flag(status_dot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(status_dot_, LV_OBJ_FLAG_HIDDEN);

    unread_mark_ = lv_label_create(screen);
    lv_label_set_text(unread_mark_, LV_SYMBOL_BELL);
    lv_obj_set_pos(unread_mark_, made_x(320), made_y(10));
    lv_obj_set_style_text_font(unread_mark_, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(unread_mark_, lv_color_hex(VT(accent)), 0);
    lv_obj_clear_flag(unread_mark_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(unread_mark_, LV_OBJ_FLAG_HIDDEN);

    position_label_ = lv_label_create(screen);
    lv_obj_set_pos(position_label_, made_x(50), made_y(154));
    lv_label_set_long_mode(position_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(position_label_, lv_color_hex(VT(text_accent)), 0);
    lv_obj_set_style_text_font(position_label_, text_font, 0);
    made_text(position_label_, 260, false);

    session_title_tap_ = lv_obj_create(screen);
    lv_obj_set_pos(session_title_tap_, made_x(45), made_y(150));
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
    lv_obj_set_style_text_color(meta_label_, lv_color_hex(VT(text_primary)), 0);
    lv_obj_set_style_text_font(meta_label_, &lv_font_montserrat_26, 0);
    made_text(meta_label_, 230, false);
    lv_obj_add_flag(meta_label_, LV_OBJ_FLAG_HIDDEN);

    instruction_label_ = lv_label_create(screen);
    lv_obj_set_pos(instruction_label_, made_x(50), made_y(169));
    lv_label_set_long_mode(instruction_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(instruction_label_, lv_color_hex(VT(text_answer)), 0);
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
    lv_obj_set_style_text_color(result_label_, lv_color_hex(VT(title)), 0);
    lv_obj_set_style_text_font(result_label_, text_font, 0);
    made_text(result_label_, 196, false);
    lv_obj_set_style_text_line_space(result_label_, 4, 0);
    if (made_rect() && made_fit() < made_design) {
        // The 360 coordinates leave less than one glyph between these lines.
        // Stack them at the real font height so 16 px text stays sharp and apart.
        const int margin = 6;
        const int width = made_screen_w() - margin * 2;
        int y = made_y(28) + made_s(74) + 6;
        auto place = [&](lv_obj_t *label, const lv_font_t *font, int gap) {
            lv_obj_set_style_text_font(label, font, 0);
            lv_obj_set_pos(label, margin, y);
            lv_obj_set_width(label, width);
            lv_obj_set_style_transform_scale_x(label, 256, 0);
            lv_obj_set_style_transform_scale_y(label, 256, 0);
            y += lv_font_get_line_height(font) + gap;
        };
        // provider_label_ 是主页精灵脚下的名字，不进这套抽屉控件堆叠
        // （旧版曾把它排到精灵身上，长屏上直接压脸）。字体仍适配小屏。
        lv_obj_set_style_text_font(provider_label_, &lv_font_montserrat_20, 0);
        place(position_label_, text_font, 2);
        lv_obj_set_height(position_label_, lv_font_get_line_height(text_font) + 2);
        lv_obj_set_pos(session_title_tap_, margin, y - lv_font_get_line_height(text_font) - 2);
        lv_obj_set_size(session_title_tap_, width, lv_font_get_line_height(text_font) + 4);
        place(instruction_label_, text_font, 4);
        // 同抽屉：固定行高让 DOT 截断生效，长指令不再换行压进回显区。
        lv_obj_set_height(instruction_label_, lv_font_get_line_height(text_font) + 2);
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
    lv_obj_set_style_bg_color(bridge_picker_, lv_color_hex(VT(bg_page)), 0);
    lv_obj_set_style_bg_opa(bridge_picker_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bridge_picker_, 0, 0);
    lv_obj_set_style_pad_all(bridge_picker_, 0, 0);
    lv_obj_remove_flag(bridge_picker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bridge_picker_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *bridge_settings = touchButton(bridge_picker_, 48, 292, 120, 36, "settings.short", settingsCallback, this);
    lv_obj_t *bridge_title = settingsLabel(bridge_picker_, T("选择电脑"), 70, 36, 220);
    bridge_refresh_button_ = touchButton(bridge_picker_, 192, 292, 120, 36,
                                          T("刷新"), bridgeRefreshCallback, this, VT(accent));
    (void)bridge_settings;
    (void)bridge_title;
    bridge_list_ = lv_obj_create(bridge_picker_);
    lv_obj_set_pos(bridge_list_, made_page_x(45), made_page_y(88));
    lv_obj_set_size(bridge_list_, made_page_w(270), made_page_y(248) - made_page_y(88));
    lv_obj_set_style_bg_opa(bridge_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bridge_list_, 0, 0);
    lv_obj_set_style_pad_all(bridge_list_, 0, 0);
    lv_obj_set_scroll_dir(bridge_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(bridge_list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(bridge_list_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    bridge_hint_ = settingsLabel(bridge_picker_, T("正在扫描电脑…"), 76, 252, 208, VT(text_accent));
    lv_obj_add_flag(bridge_picker_, LV_OBJ_FLAG_HIDDEN);
    bridge_list_revision_ = UINT32_MAX;
    bridge_rows_.clear();

    // 抽屉：任务管理、文字回显、会话与设置都收在这里。主页上滑唤出，
    // 抽屉内下拉或返回键收起。创建在 delete_dialog_ 之前，保证弹窗永远更高。
    drawer_ = lv_obj_create(screen);
    lv_obj_set_pos(drawer_, made_s(0), made_s(0));
    lv_obj_set_size(drawer_, made_screen_w(), made_screen_h());
    markPage(drawer_);
    lv_obj_set_style_bg_color(drawer_, lv_color_hex(VT(bg_page)), 0);
    lv_obj_set_style_bg_opa(drawer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(drawer_, 0, 0);
    lv_obj_set_style_pad_all(drawer_, 0, 0);
    lv_obj_remove_flag(drawer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(drawer_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(drawer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(drawer_, drawerSwipeCallback, LV_EVENT_GESTURE, this);
    lv_obj_add_flag(drawer_, LV_OBJ_FLAG_HIDDEN);

    // 顶部把手与提示：告知抽屉可下拉收起。
    drawer_grip_ = lv_obj_create(drawer_);
    lv_obj_remove_style_all(drawer_grip_);
    lv_obj_set_pos(drawer_grip_, made_x(150), made_y(8));
    lv_obj_set_size(drawer_grip_, made_s(60), made_s(6));
    lv_obj_set_style_radius(drawer_grip_, 3, 0);
    lv_obj_set_style_bg_color(drawer_grip_, lv_color_hex(VT(text_hint)), 0);
    lv_obj_set_style_bg_opa(drawer_grip_, LV_OPA_70, 0);
    lv_obj_clear_flag(drawer_grip_, LV_OBJ_FLAG_CLICKABLE);
    // 手势类提示（「下拉收起 · 左右滑切任务」）已按设计移除：滑不出就试不出来，
    // 这类文案只占一行空间还教坏用户。

    // 把平铺在主页的任务/回显/会话/设置控件整体搬进抽屉，重排为抽屉版式。
    // 纵向用 made_page_y 全高映射：矩形竖屏上内容铺满屏，不再挤在上半截。
    // 行序：提示 / 任务行(chip+徽章+标题) / 指令 / 活动条 / 回显。
    // 左缘与标题行（x24）对齐：卡片内文字 = 卡片 12 + 内边距 12 = 24，
    // 三层文本共用一条左基线（真机上「你：」比标题探出 8 设计像素，2026-10-17）。
    lv_obj_set_parent(instruction_label_, drawer_);
    lv_obj_set_pos(instruction_label_, made_x(24), made_page_y(104));
    lv_obj_set_width(instruction_label_, made_s(312));
    // 指令行固定一行高（设计定稿）：超长 LONG_DOT 截断，省出的一行让给回显卡片。
    lv_obj_set_height(instruction_label_, lv_font_get_line_height(drawer_text_font) + 2);
    lv_obj_set_style_text_font(instruction_label_, drawer_text_font, 0);
    // 抽屉版式全部左对齐（创建时是主页居中排版，不重设会叠出错位观感）。
    lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_parent(position_label_, drawer_);
    lv_obj_set_pos(position_label_, made_x(24), made_page_y(78));
    lv_obj_set_width(position_label_, made_s(312));
    lv_obj_set_height(position_label_, lv_font_get_line_height(text_font) + 2);
    // 标题行改为左对齐的纯标题（任务序号挪进 chip，状态挪进徽章）。
    lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(position_label_, lv_color_hex(VT(text_primary)), 0);
    lv_obj_set_parent(session_title_tap_, drawer_);
    lv_obj_set_pos(session_title_tap_, made_x(24), made_page_y(40));
    lv_obj_set_size(session_title_tap_, made_s(312), made_s(60));
    lv_obj_set_parent(answer_area_, drawer_);
    applyDrawerAnswerLayout();
    lv_obj_set_parent(meta_label_, drawer_);
    lv_obj_set_pos(meta_label_, made_x(30), made_page_y(256));
    lv_obj_set_width(meta_label_, made_s(300));
    lv_obj_set_parent(new_session_button_, drawer_);
    lv_obj_set_pos(new_session_button_, made_x(270), made_page_y(296));
    lv_obj_set_parent(delete_session_button_, drawer_);
    lv_obj_set_pos(delete_session_button_, made_x(157), made_page_y(296));
    lv_obj_set_parent(settings_button_, drawer_);
    lv_obj_set_pos(settings_button_, made_x(44), made_page_y(296));

    // 分页 chip（左上）：「任务 1/3」，从标题行剥离出来。
    // 分页 chip（左上）：「任务 1/3」，从标题行剥离出来。
    // 高度随字号行高走，宽度随内容自适应。
    const int chip_h = lv_font_get_line_height(text_font) + made_s(8);
    session_chip_ = lv_obj_create(drawer_);
    lv_obj_remove_style_all(session_chip_);
    lv_obj_set_pos(session_chip_, made_x(24), made_page_y(44));
    lv_obj_set_size(session_chip_, LV_SIZE_CONTENT, chip_h);
    lv_obj_set_style_pad_left(session_chip_, made_s(10), 0);
    lv_obj_set_style_pad_right(session_chip_, made_s(10), 0);
    lv_obj_set_style_radius(session_chip_, chip_h / 2, 0);
    lv_obj_set_style_bg_color(session_chip_, lv_color_hex(VT(bg_row)), 0);
    lv_obj_set_style_bg_opa(session_chip_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(session_chip_, LV_OBJ_FLAG_CLICKABLE);
    session_chip_label_ = lv_label_create(session_chip_);
    lv_obj_set_style_text_color(session_chip_label_, lv_color_hex(VT(text_primary)), 0);
    lv_obj_set_style_text_font(session_chip_label_, text_font, 0);
    lv_obj_center(session_chip_label_);
    lv_obj_remove_flag(session_chip_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(session_chip_, LV_OBJ_FLAG_HIDDEN);

    // 状态徽章（右上）：状态点 + 状态词，双通道传达（不只靠颜色）。
    // 高度随字号行高、宽度随状态词自适应。
    const int badge_h = lv_font_get_line_height(text_font) + made_s(8);
    status_badge_ = lv_obj_create(drawer_);
    lv_obj_remove_style_all(status_badge_);
    // 宽度随状态词自适应（SIZE_CONTENT），右缘对齐在 renderStatusBadge 里
    // 文字落定后再定位（创建时宽度还是 0，算不准）。
    lv_obj_set_pos(status_badge_, made_x(218), made_page_y(44));
    lv_obj_set_size(status_badge_, LV_SIZE_CONTENT, badge_h);
    lv_obj_set_style_pad_left(status_badge_, made_s(10), 0);
    lv_obj_set_style_pad_right(status_badge_, made_s(12), 0);
    lv_obj_set_style_radius(status_badge_, badge_h / 2, 0);
    lv_obj_set_style_bg_color(status_badge_, lv_color_hex(VT(bg_row)), 0);
    lv_obj_set_style_bg_opa(status_badge_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(status_badge_, LV_OBJ_FLAG_CLICKABLE);
    status_badge_dot_ = lv_obj_create(status_badge_);
    lv_obj_remove_style_all(status_badge_dot_);
    lv_obj_set_size(status_badge_dot_, made_s(8), made_s(8));
    lv_obj_align(status_badge_dot_, LV_ALIGN_LEFT_MID, made_s(12), 0);
    lv_obj_set_style_radius(status_badge_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(status_badge_dot_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(status_badge_dot_, LV_OBJ_FLAG_CLICKABLE);
    status_badge_label_ = lv_label_create(status_badge_);
    lv_obj_set_style_text_font(status_badge_label_, text_font, 0);
    lv_obj_align(status_badge_label_, LV_ALIGN_LEFT_MID, made_s(28), 0);
    lv_obj_remove_flag(status_badge_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(status_badge_, LV_OBJ_FLAG_HIDDEN);

    // 活动条（不确定式）：只在任务运行时扫动，表达"进行中"。
    // 任务链路没有百分比信号（progress 是自由文本），绝不做确定性进度条。
    activity_bar_ = lv_obj_create(drawer_);
    lv_obj_remove_style_all(activity_bar_);
    lv_obj_set_pos(activity_bar_, made_x(12), made_page_y(122));
    lv_obj_set_size(activity_bar_, made_s(336), made_s(5));
    lv_obj_set_style_radius(activity_bar_, made_s(2), 0);
    lv_obj_set_style_bg_color(activity_bar_, lv_color_hex(VT(bg_row)), 0);
    lv_obj_set_style_bg_opa(activity_bar_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(activity_bar_, LV_OBJ_FLAG_CLICKABLE);
    activity_fill_ = lv_obj_create(activity_bar_);
    lv_obj_remove_style_all(activity_fill_);
    lv_obj_set_size(activity_fill_, made_s(64), made_s(5));
    lv_obj_set_style_radius(activity_fill_, made_s(2), 0);
    lv_obj_set_style_bg_color(activity_fill_, lv_color_hex(VT(accent)), 0);
    lv_obj_set_style_bg_opa(activity_fill_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(activity_fill_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(activity_bar_, LV_OBJ_FLAG_HIDDEN);

    delete_dialog_ = lv_obj_create(screen);
    lv_obj_set_pos(delete_dialog_, made_s(0), made_s(0));
    lv_obj_set_size(delete_dialog_, made_screen_w(), made_screen_h());
    markPage(delete_dialog_);
    lv_obj_set_style_bg_color(delete_dialog_, lv_color_hex(VT(bg_page)), 0);
    lv_obj_set_style_bg_opa(delete_dialog_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(delete_dialog_, 0, 0);
    lv_obj_set_style_pad_all(delete_dialog_, 0, 0);
    lv_obj_remove_flag(delete_dialog_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(delete_dialog_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    settingsLabel(delete_dialog_, T("删除当前任务？"), 55, 88, 250);
    delete_dialog_title_ = settingsLabel(delete_dialog_, "", 55, 129, 250, VT(text_accent), false);
    lv_label_set_long_mode(delete_dialog_title_, LV_LABEL_LONG_DOT);
    settingsLabel(delete_dialog_, T("任务及对话记录将被删除"), 55, 171, 250, VT(text_hint));
    touchButton(delete_dialog_, 48, 292, 120, 36, T("返回"), deleteSessionCancelCallback, this);
    touchButton(delete_dialog_, 192, 292, 120, 36, T("删除"), deleteSessionConfirmCallback, this, VT(danger));
    lv_obj_add_flag(delete_dialog_, LV_OBJ_FLAG_HIDDEN);

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

// —— 回复区富内容：图片 / 链接二维码 + 点击放大浮层 ——

// 释放上一轮解码出来的图片位图，避免泄漏与悬垂。
void VibeCoding::freeDecodedImages() {
    for (auto *dsc : decoded_dscs_) {
        if (dsc) {
            heap_caps_free(const_cast<uint8_t *>(dsc->data));
            delete dsc;
        }
    }
    decoded_dscs_.clear();
    attachment_dscs_.clear();
}

void VibeCoding::renderAttachments(const std::vector<Attachment> &atts) {
    // 指纹：仅当附件集合（类型+URL+文案）变化时重建，避免每次 render 重拉图/闪屏。
    std::string sig;
    for (const auto &a : atts) sig += a.type + '\x1f' + a.url + '\x1f' + a.text + '\x1e';
    // 放大浮层打开期间：绝不重建、绝不释放 dsc（浮层图像正引用它），直接保留现状。
    if (zoom_overlay_) return;
    if (sig == attachment_sig_ && attachments_box_) return; // 无变化，维持已渲染内容
    // 集合已变（或首次）：先清旧内容再按新集合重建。
    for (auto *o : attachment_objs_) lv_obj_del(o);
    attachment_objs_.clear();
    freeDecodedImages();
    attachment_sig_ = sig;
    current_attachments_ = atts;
    attachment_dscs_.assign(atts.size(), nullptr);
    if (!answer_area_ || atts.empty()) {
        if (attachments_box_) lv_obj_add_flag(attachments_box_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_update_layout(answer_area_);
    const int label_h = lv_obj_get_height(result_label_);
    const int box_w = lv_obj_get_width(answer_area_) - 12;
    if (!attachments_box_) {
        attachments_box_ = lv_obj_create(answer_area_);
        lv_obj_set_style_bg_opa(attachments_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(attachments_box_, 0, 0);
        lv_obj_set_style_pad_all(attachments_box_, 0, 0);
        lv_obj_set_style_pad_row(attachments_box_, 8, 0);
        lv_obj_set_flex_flow(attachments_box_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(attachments_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_remove_flag(attachments_box_, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_pos(attachments_box_, 6, label_h + 8);
    lv_obj_set_size(attachments_box_, box_w, LV_SIZE_CONTENT);
    lv_obj_clear_flag(attachments_box_, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < atts.size(); ++i) {
        const Attachment &a = atts[i];
        lv_obj_t *row = lv_obj_create(attachments_box_);
        lv_obj_set_size(row, box_w, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(VT(btn_normal)), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 5, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_user_data(row, reinterpret_cast<void *>(i));  // 用索引回查 current_attachments_
        lv_obj_add_event_cb(row, attachmentTapCallback, LV_EVENT_CLICKED, this);
        attachment_objs_.push_back(row);

        if (a.type == "image") {
            lv_obj_t *img = lv_image_create(row);
            lv_obj_set_size(img, 84, 84);
            lv_obj_t *cap = lv_label_create(row);
            lv_label_set_text(cap, "图片加载中…");
            lv_obj_set_width(cap, box_w - 100);
            lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(cap, lv_color_hex(VT(text_hint)), 0);
            startImageFetch(a.url, img, i);
        } else {
            lv_obj_t *qr = lv_qrcode_create(row);
            lv_qrcode_set_size(qr, 84);
            lv_qrcode_set_dark_color(qr, lv_color_black());
            lv_qrcode_set_light_color(qr, lv_color_white());
            lv_qrcode_set_quiet_zone(qr, true);
            lv_qrcode_update(qr, a.url.c_str(), a.url.size());
            lv_obj_t *cap = lv_label_create(row);
            lv_label_set_text(cap, a.text.empty() ? a.url.c_str() : a.text.c_str());
            lv_obj_set_width(cap, box_w - 100);
            lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(cap, lv_color_hex(VT(text_hint)), 0);
        }
    }
    lv_obj_update_layout(answer_area_);
}

void VibeCoding::attachmentTapCallback(lv_event_t *event) {
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    lv_obj_t *row = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const size_t idx = reinterpret_cast<size_t>(lv_obj_get_user_data(row));
    if (idx < self->current_attachments_.size()) self->openZoom(idx);
}

void VibeCoding::zoomCloseCallback(lv_event_t *event) {
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    if (self->zoom_overlay_) {
        lv_obj_del(self->zoom_overlay_);
        self->zoom_overlay_ = nullptr;
    }
}

void VibeCoding::openZoom(size_t idx) {
    if (idx >= current_attachments_.size()) return;
    const Attachment &a = current_attachments_[idx];
    if (zoom_overlay_) lv_obj_del(zoom_overlay_);
    zoom_overlay_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(zoom_overlay_, made_screen_w(), made_screen_h());
    lv_obj_set_style_bg_color(zoom_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(zoom_overlay_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(zoom_overlay_, 0, 0);
    lv_obj_set_style_pad_all(zoom_overlay_, 0, 0);
    lv_obj_remove_flag(zoom_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(zoom_overlay_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(zoom_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zoom_overlay_, zoomCloseCallback, LV_EVENT_CLICKED, this);

    lv_obj_t *content = nullptr;
    if (a.type == "image") {
        lv_obj_t *img = lv_image_create(zoom_overlay_);
        // 按索引取本附件对应的已解码图（dsc 由 freeDecodedImages 托管生命周期；
        // 浮层打开期间不会被清理，因为 renderAttachments 在重建前会先关掉旧浮层）。
        lv_image_dsc_t *dsc = (idx < attachment_dscs_.size()) ? attachment_dscs_[idx] : nullptr;
        if (dsc) {
            lv_image_set_src(img, dsc);
            lv_obj_set_size(img, std::min<int>(made_screen_w() - 20, static_cast<int>(dsc->header.w)),
                                  std::min<int>(made_screen_h() - 60, static_cast<int>(dsc->header.h)));
            content = img;
        } else {
            lv_label_set_text(lv_label_create(zoom_overlay_),
                ("图片加载中或不可用\n" + a.url).c_str());
        }
    } else {
        lv_obj_t *qr = lv_qrcode_create(zoom_overlay_);
        lv_qrcode_set_size(qr, std::min(made_screen_w() - 24, 280));
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        lv_qrcode_update(qr, a.url.c_str(), a.url.size());
        content = qr;
    }
    if (content) lv_obj_center(content);
    lv_obj_t *hint = lv_label_create(zoom_overlay_);
    lv_label_set_text(hint, "点击任意处关闭");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

// 图片附件：后台拉取 + 解码，完成后切回 LVGL 线程设置到目标 lv_image。
struct ImageFetchArg {
    std::string url;
    lv_obj_t *img;
    VibeCoding *self;
    size_t index; // 对应 current_attachments_ 中的序号，用于回写 attachment_dscs_
    std::vector<uint8_t> buf;
};

// lv_async_call 的回调必须是自由函数（非成员），签名为 void(*)(void*)。
// 解码由本函数完成，建 dsc / 写私有成员交给成员方法 onImageFetched（可访问私有数据）。
static void imageFetchDoneCb(void *arg_void) {
    auto *arg = static_cast<ImageFetchArg *>(arg_void);
    if (!arg || !arg->self) { delete arg; return; }
    if (arg->img && lv_obj_is_valid(arg->img) && !arg->buf.empty()) {
        int w = 0, h = 0;
        uint8_t *px = made_sprite::decodePngArgbMem(arg->buf.data(), arg->buf.size(), w, h);
        if (px) {
            // 缩略图只有 84px，放大也不超过屏幕。按最长边收到 360，避免一张照片按原尺寸占满 PSRAM。
            const int cap = std::max(84, std::min(made_screen_w(), 360));
            if (w > cap || h > cap) {
                const int dw = w >= h ? cap : std::max(1, w * cap / h);
                const int dh = h >= w ? cap : std::max(1, h * cap / w);
                auto *dst = static_cast<uint8_t *>(heap_caps_malloc(
                    static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (dst) {
                    for (int y = 0; y < dh; ++y) {
                        const int sy = y * h / dh;
                        for (int x = 0; x < dw; ++x) {
                            const int sx = x * w / dw;
                            memcpy(dst + (static_cast<size_t>(y) * dw + x) * 4,
                                   px + (static_cast<size_t>(sy) * w + sx) * 4, 4);
                        }
                    }
                    heap_caps_free(px);
                    px = dst;
                    w = dw;
                    h = dh;
                }
            }
            arg->self->onImageFetched(arg->index, px, w, h, arg->img);
        }
    }
    delete arg;
}

void VibeCoding::onImageFetched(size_t index, uint8_t *px, int w, int h, lv_obj_t *img) {
    auto *dsc = new lv_image_dsc_t();
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w = static_cast<uint32_t>(w);
    dsc->header.h = static_cast<uint32_t>(h);
    dsc->header.stride = static_cast<uint32_t>(w) * 4;
    dsc->data_size = static_cast<uint32_t>(w) * h * 4;
    dsc->data = px;
    decoded_dscs_.push_back(dsc);
    // 回写按索引映射：异步回调可能晚于下一次 render，需防越界。
    if (index < attachment_dscs_.size()) attachment_dscs_[index] = dsc;
    if (img && lv_obj_is_valid(img)) {
        lv_image_set_src(img, dsc);
        lv_obj_set_size(img, 84, 84);
    }
}

void VibeCoding::startImageFetch(const std::string &url, lv_obj_t *img, size_t index) {
    auto *arg = new ImageFetchArg{url, img, this, index, {}};
    if (xTaskCreate([](void *p) {
            auto *a = static_cast<ImageFetchArg *>(p);
            a->buf = a->self->fetchUrlBytes(a->url, 512 * 1024);
            lv_async_call(imageFetchDoneCb, a);
            vTaskDelete(nullptr);
        }, "img_fetch", 8192, arg, 2, nullptr) != pdPASS) {
        delete arg;
    }
}

// 直接拉取任意 http(s) URL 的字节（图片附件用）。返回空表示失败。
std::vector<uint8_t> VibeCoding::fetchUrlBytes(const std::string &url, size_t max) {
    std::vector<uint8_t> out;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 4096;
    auto *client = esp_http_client_init(&cfg);
    if (!client) return out;
    if (esp_http_client_open(client, 0) != ESP_OK) { esp_http_client_cleanup(client); return out; }
    const int len = esp_http_client_fetch_headers(client);
    if (len < 0) { esp_http_client_cleanup(client); return out; }
    int total = 0;
    char buf[1024];
    int r;
    while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        if (static_cast<size_t>(total + r) > max) { esp_http_client_cleanup(client); return out; }
        out.insert(out.end(), buf, buf + r);
        total += r;
    }
    esp_http_client_cleanup(client);
    return out;
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
    if (sprite_stage_) {
        std::string task_status;
        std::string connection_error;
        std::string selected_provider;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            connection_error = model_.connection_error;
            if (!model_.tasks.empty()) task_status = model_.tasks.front().status;
            selected_provider = selectedProviderLocked().id;
        }
        // 助手切换即刻反映到素材（撤旧、请求 worker 下载新的）。
        if (artwork_seen_provider_ != selected_provider) {
            artwork_seen_provider_ = selected_provider;
            artwork_request_.store(true);
            ESP_LOGI(BUTTON_TAG, "[sprite] provider_changed provider=%s -> request",
                selected_provider.c_str());
        }
        maybeApplyArtwork(selected_provider);
        // 瞬时情绪：盯最前面的任务，进行中 → 终态的翻转换来 ~3.5s 的
        // 成功/失败表情。素材没配对应 emotions 条目时，固件只是动效节奏
        // 略有不同，安全无害。
        {
            std::string watch_id;
            std::string watch_status;
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                if (!model_.tasks.empty()) {
                    watch_id = model_.tasks.front().id;
                    watch_status = model_.tasks.front().status;
                }
            }
            if (watch_id != sprite_watch_id_) {
                sprite_watch_id_ = watch_id;
                sprite_watch_status_ = watch_status;
                sprite_transient_mood_ = -1;   // 新任务：旧表情立刻让位
            } else if (watch_status != sprite_watch_status_) {
                const bool was_pending = sprite_watch_status_ == "queued" ||
                    sprite_watch_status_ == "running" ||
                    sprite_watch_status_ == "waiting_confirmation";
                if (was_pending && watch_status == "completed") {
                    sprite_transient_mood_ = made_sprite::Mood::kSuccess;
                    sprite_transient_until_ = lv_tick_get() + 3500;
                } else if (was_pending && (watch_status == "failed" ||
                                           watch_status == "cancelled")) {
                    sprite_transient_mood_ = made_sprite::Mood::kSad;
                    sprite_transient_until_ = lv_tick_get() + 3500;
                }
                sprite_watch_status_ = watch_status;
            }
            const bool pending_now = watch_status == "queued" ||
                watch_status == "running" || watch_status == "waiting_confirmation";
            if (pending_now) sprite_transient_mood_ = -1;   // 又有活干：表情收起
        }
        int mood = made_sprite::Mood::kIdle;
        if (!connection_error.empty()) mood = made_sprite::Mood::kOffline;
        else if (voice.phase == vibe_voice::Phase::Recording || voice.phase == vibe_voice::Phase::Uploading)
            mood = made_sprite::Mood::kTalking;
        else if (sprite_transient_mood_ >= 0 &&
                 static_cast<int32_t>(lv_tick_get() - sprite_transient_until_) < 0)
            mood = sprite_transient_mood_;
        else if (task_status == "waiting_confirmation") mood = made_sprite::Mood::kWaitingConfirm;
        else if (task_status == "running" || task_status == "queued") mood = made_sprite::Mood::kRunning;
        made_sprite::update(sprite_stage_, lv_tick_get() - sprite_started_ms_, mood);
        // 侧台同步驱动：拖动中滑入视野的素材也是活的（呼吸/眨眼/轻摆），
        // 不是一张死图。烘焙已对侧台关闭，走动态缩放路径。
        for (auto &slot : sides_)
            if (slot.stage)
                made_sprite::update(slot.stage, lv_tick_get() - sprite_started_ms_, mood);
        // 素材页顶部状态图标与精灵同一状态源（voice phase + 最前任务），
        // 随帧刷新；抽屉/设置打开时内部自隐藏。
        updateStatusIcon(static_cast<int>(voice.phase), task_status, !connection_error.empty());
    }
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
        lv_obj_remove_flag(page_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(page_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(provider_label_, text_font, 0);
        lv_label_set_text(provider_label_, catalog_ready ? T("暂无助手") : T("连接电脑"));
    }
    // 顶部状态角标跟目录/连接状态每帧刷新（未配对时 catalog_ready 为假，自动隐藏）。
    updateStatusIndicators(catalog_ready && provider_count != 0);

    if (pairing.phase != vibe_pairing::Phase::Paired ||
        !vibe_pairing::authorized_for_foreground()) {
        lv_obj_add_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
        displayed_task_id_.clear();
        // 配对码显示在抽屉里：未配对期间保持抽屉拉开，用户随时能看到六位码。
        // 记住是这里自动拉开的，配对完成后自动收囘，开机不拐进抽屉。
        if (!drawer_open_.load()) {
            showDrawer(true);
            pairing_drawer_auto_ = true;
        }
        if (!showing_pairing_layout_) {
            lv_obj_remove_flag(meta_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(answer_area_, made_x(24), made_page_y(196));
            lv_obj_set_size(answer_area_, made_s(312), made_page_h(58));
            lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_CENTER, 0);
            // 配对版式沿用主页的居中排版（抽屉构建处已把这些行改成左对齐）。
            lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_CENTER, 0);
            showing_pairing_layout_ = true;
        }
        const size_t scheme = pairing.url.find("://");
        const std::string target = pairing.url.empty() ? std::string() :
            pairing.url.substr(scheme == std::string::npos ? 0 : scheme + 3);
        const bool usb_direct = pairing.access_mode == vibe_pairing::AccessMode::UsbDirect;
        // 位置行只在有独立信息时出现（USB 直连 / 已定目标）；否则与下面
        // 的引导行重复成两行「正在查找电脑」（实机截图 bug）。
        const std::string computer = usb_direct ? T("USB 直连电脑") :
            target.empty() ? std::string() : T("目标 ") + target;
        if (computer.empty()) {
            lv_obj_add_flag(position_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(position_label_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(position_label_, computer.c_str());
        }
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

    if (pairing_drawer_auto_ && drawer_open_.load()) {
        pairing_drawer_auto_ = false;
        showDrawer(false);
    }
    if (catalog_ready && provider_count) lv_obj_remove_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
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
    if (!catalog_ready || !provider_count) {
        lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
        displayed_task_id_.clear();
        rendered_session_id_.clear();
        lv_label_set_text(position_label_, catalog_ready ? T("暂无助手") : T("读取助手列表"));
        lv_label_set_text(instruction_label_, catalog_ready ? T("请在电脑端添加助手插件") : T("正在读取电脑设置"));
        // 状态类短文案与上方居中的标题行同排版，避免「贴左孤行」的观感。
        lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_CENTER, 0);
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
        lv_obj_add_flag(session_chip_, LV_OBJ_FLAG_HIDDEN);
        renderStatusBadge("");
        setActivityRunning(false);
        lv_label_set_text(position_label_, T("暂无任务"));
        lv_label_set_text(instruction_label_, !snapshot.session_notice.empty() ? own_session_notice.c_str() : T("点 + 创建任务"));
        lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_CENTER, 0);
        const std::string empty_message = !snapshot.connection_error.empty() ? own_error :
            !snapshot.session_notice.empty() ? own_session_notice :
            !provider_ready ? (provider.reason.empty() ? T("请先在电脑端启用此助手") : provider.reason) :
            T("先创建任务，再按住 BOOT 说话。");
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
        std::to_string(snapshot.sessions.size());
    // 分页 chip 只承载序号，标题行只承载标题（详见抽屉重排注释）。
    lv_label_set_text(session_chip_label_, session_header.c_str());
    lv_obj_remove_flag(session_chip_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(position_label_, selected_session->title.empty() ? T("未命名任务") : selected_session->title.c_str());
    // 任务回显是长文，恢复左对齐（catalog/空会话状态曾切成居中）。
    lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_LEFT, 0);
    ui_stage_.store(20);

    if (voice_for_selected_session && voice.phase == vibe_voice::Phase::Submitted && !voice.task_id.empty() &&
        !submitted_task_seen_.load()) {
        const auto submitted = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
            [&voice](const Task &task) { return task.id == voice.task_id; });
        if (submitted == snapshot.tasks.end()) {
            // Do not leave an older actionable exchange on screen after the 201.
            displayed_task_id_.clear();
            renderStatusBadge("");
            setActivityRunning(false);
            lv_label_set_text(instruction_label_, T("新一轮已发送"));
            showAnswer(selected_session_id + "/submitted", T("正在读取刚发送的内容。三击 BOOT 可取消，双击请等回显。"));
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                visible_task_id_.clear();
            }
            renderAttachments({}); // 提交态无回显，清空可能残留的旧附件（放大浮层打开时安全跳过）
            layoutReadableText();
            drawn_revision_ = snapshot.revision;
            return;
        }
    }

    if (snapshot.tasks.empty()) {
        displayed_task_id_.clear();
        renderStatusBadge("");
        setActivityRunning(false);
        lv_label_set_text(instruction_label_, !snapshot.session_notice.empty() ? own_session_notice.c_str() :
                          !voice_notice.empty() ? voice_notice.c_str() :
                          provider_ready ? T("还没有对话 · 按住 BOOT 说话") : T("电脑端助手不可用"));
        const std::string result = !snapshot.connection_error.empty() ? own_error :
            !config_loaded_.load() ? T("正在读取电脑设置") :
            !snapshot.session_notice.empty() ? own_session_notice :
            !voice_available_.load() ? T("在电脑工作台设置语音识别") :
            voice.phase == vibe_voice::Phase::Recording ?
                (voice.hold_mode ? T("按住说话中，松手即发送。") : T("说完后稍等，会自动发送。")) :
            voice.phase == vibe_voice::Phase::Uploading ? T("正在识别语音…") :
            !voice_notice.empty() ? voice_notice :
            provider.external_unscoped ? T("此助手任务隔离受限，请在电脑端核对。") :
            T("按住 BOOT 说话，发起新任务。");
        showAnswer(selected_session_id + "/empty", result);
        renderAttachments({}); // 无任务时清空可能残留的旧附件
    } else {
        // The bridge returns newest first. Show only the latest exchange.
        const Task &task = snapshot.tasks.front();
        displayed_task_id_ = task.id;
        noteComputerReply(task);
        // 状态已由徽章承载，指令行不再重复拼接状态词。
        const std::string instruction = !snapshot.session_notice.empty() ? own_session_notice :
            !voice_notice.empty() ? voice_notice :
            std::string(T("你：")) + task.instruction;
        renderStatusBadge(task.status);
        setActivityRunning(task.status == "running");
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
        if (!snapshot.session_notice.empty()) result = own_session_notice + (result.empty() ? std::string() : "\n\n" + result);
        lv_label_set_text(instruction_label_, instruction.c_str());
        ui_stage_.store(30);
        showAnswer(selected_session_id + "/" + task.id + "/" + task.status,
                   result);
        // 富内容：仅在实际 AI 回显（非反馈/错误/排队）时渲染；其它状态清空旧附件。
        if (showing_task_result) {
            renderAttachments(task.attachments.empty() ? parseUrlAttachments(task.result)
                                                       : task.attachments);
        } else {
            renderAttachments({});
        }
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
    // 实测）。所以任务状态可见时一律以任务为准，Submitted 只兜底「任务
    // 还没出现在快照里」的窗口期。
    if (voice_phase == static_cast<int>(vibe_voice::Phase::Recording)) state = 1;
    else if (voice_phase == static_cast<int>(vibe_voice::Phase::Uploading)) state = 2;
    else if (task_status == "waiting_confirmation") state = 3;
    else if (task_status == "running" || task_status == "queued") state = 4;
    else if (voice_phase == static_cast<int>(vibe_voice::Phase::Submitted) && task_status.empty())
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

struct HttpCapture {
    std::string *body = nullptr;
    size_t limit = 0;
};

esp_err_t VibeCoding::httpEvent(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || event->data_len <= 0) return ESP_OK;
    auto *capture = static_cast<HttpCapture *>(event->user_data);
    if (!capture->body || capture->body->size() + static_cast<size_t>(event->data_len) > capture->limit) return ESP_FAIL;
    capture->body->append(static_cast<const char *>(event->data), event->data_len);
    return ESP_OK;
}

bool VibeCoding::request(const std::string &path, bool post, std::string &response, int &status,
                         const std::string &json_body, size_t response_limit, int timeout_ms)
{
    if (!post && receiverVoiceUploading()) return false;
    const auto pairing = vibe_pairing::snapshot();
#if VIBE_SMOKE_TEST
    // 冒烟测试：设备侧配对记录可能为空，伪造凭据（USB direct 通道桥接器
    // 侧物理信任，token 不参与校验）。
    std::string base = pairing.url.empty() ? "http://burnin.local" : pairing.url;
    const std::string token = pairing.token.empty() ? "baeeabde0ae90cbff6e590641c6d91827c537228d7e56f862fc5244cc7596f05" : pairing.token;
#else
    std::string base = pairing.url;
    const std::string token = pairing.token;
#endif
    if (base.empty() || token.empty() || !vibe_pairing::authorized_for_foreground()) {
#if VIBE_SMOKE_TEST
        ESP_LOGW("vibe_req", "[req-gate] base=%d token=%d auth=%d", !base.empty(), !token.empty(),
                 vibe_pairing::authorized_for_foreground());
#endif
        return false;
    }
#if !VIBE_SMOKE_TEST
    if (base.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) {
        setConnectionError(vibe_pairing::tls_time_failed() ?
            T("设备时间同步失败，无法校验 HTTPS 证书") : T("正在同步设备时间，以校验 HTTPS 证书"));
        return false;
    }
#endif
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string wire_path = vibe_i18n::request_path(path);
    const std::string url = base + wire_path;
    const std::string authorization = "Bearer " + token;
    esp_err_t error = ESP_FAIL;
    if (VIBE_SMOKE_TEST ||
        pairing.access_mode == vibe_pairing::AccessMode::UsbDirect) {
        error = vibe_usb::request(wire_path, post, authorization,
                                  json_body.empty() ? "" : "application/json",
                                  json_body.empty() ? nullptr : reinterpret_cast<const uint8_t *>(json_body.data()),
                                  json_body.size(),
                                  response, status, response_limit, timeout_ms);
#if VIBE_SMOKE_TEST
        if (error != ESP_OK)
            ESP_LOGW("vibe_req", "[req-usb] %s err=%d", wire_path.c_str(), error);
#endif
    } else if (pairing.access_mode == vibe_pairing::AccessMode::Receiver) {
        error = vibe_usb::receiver_request(wire_path, post, authorization,
                                            json_body.empty() ? "" : "application/json",
                                            json_body.empty() ? nullptr : reinterpret_cast<const uint8_t *>(json_body.data()),
                                            json_body.size(),
                                            response, status, response_limit, timeout_ms);
    } else {
        HttpCapture capture{&response, response_limit};
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.disable_auto_redirect = true;
        if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
        config.event_handler = httpEvent;
        config.user_data = &capture;
        config.timeout_ms = url.rfind("https://", 0) == 0 && timeout_ms < REMOTE_REQUEST_TIMEOUT_MS
                                ? REMOTE_REQUEST_TIMEOUT_MS : timeout_ms;
        config.buffer_size = 1024;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return false;
        esp_http_client_set_header(client, "ngrok-skip-browser-warning", "1");
        esp_http_client_set_header(client, "Authorization", authorization.c_str());
        if (post) esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (post && !json_body.empty()) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, json_body.c_str(), static_cast<int>(json_body.size()));
        }
        error = esp_http_client_perform(client);
        status = error == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);
    }
    const auto latest = vibe_pairing::snapshot();
#if VIBE_SMOKE_TEST
    // 压测伪造 token 与真实配对快照（token 为空）必然不等，上面的检查会把
    // 每一个成功响应都当「旧通道残 reply」丢弃——这正是 config 无限重拉、
    // providers 永不填充的根因。USB direct 物理信任，压测期直接放行。
#else
    if (!vibe_pairing::authorized_for_foreground() || latest.url != pairing.url ||
        latest.token != token) {
        status = 0;
        return false; // Discard a reply from the previous access mode.
    }
#endif
    if (error != ESP_OK) {
        invalidateCatalog(); // Re-read the catalog as soon as the bridge reconnects.
        vibe_pairing::reconnect();
    }
    return error == ESP_OK;
}

void VibeCoding::refreshConfig()
{
    const uint32_t epoch = catalog_epoch_.load();
    const auto origin = vibe_pairing::snapshot();
    std::string response;
    int status = 0;
    // config 响应含完整 provider/素材目录（实测 ~9KB），默认 8192 会被
    // receive_response 截断导致 parse 永远失败、config 无限重拉。
    if (!request("/device/config", false, response, status, "", 256 * 1024)) return;
#if VIBE_SMOKE_TEST
    ESP_LOGW("vibe_cfg", "[cfg] status=%d bytes=%u", status, static_cast<unsigned>(response.size()));
#endif
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
    std::vector<std::string> board_events;
    const cJSON *board = cJSON_GetObjectItemCaseSensitive(root, "board");
    const cJSON *event_list = cJSON_IsObject(board) ? cJSON_GetObjectItemCaseSensitive(board, "events") : nullptr;
    if (cJSON_IsArray(event_list)) {
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, event_list) {
            if (!cJSON_IsString(item) || !item->valuestring || board_events.size() >= 32) continue;
            const std::string name = item->valuestring;
            if (name.size() <= 48 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._") == std::string::npos)
                board_events.push_back(name);
        }
    }
    // announce 必须在 cJSON_Delete(root) 之前取值：删除后再读 root 是释放后使用，
    // 实测会把「桥接器重启后请重报清单」的指令整个丢掉（只有开机路径才重报）。
    const bool announce_catalog = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "announce"));
    std::string firmware_url = jsonString(root, "firmwareUrl");
    std::string firmware_sha = jsonString(root, "firmwareSha256");
    std::string firmware_version = jsonString(root, "firmwareVersion");
    const cJSON *firmware_size_item = cJSON_GetObjectItemCaseSensitive(root, "firmwareSize");
    const int firmware_size = cJSON_IsNumber(firmware_size_item) && firmware_size_item->valuedouble > 0 &&
        firmware_size_item->valuedouble <= 8 * 1024 * 1024
        ? static_cast<int>(firmware_size_item->valuedouble) : 0;
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
        board_events_ = std::move(board_events);
        if (firmware_url.size() > 180) firmware_url.resize(180);
        const bool sha_ok = firmware_sha.size() == 64 &&
            firmware_sha.find_first_not_of("0123456789abcdef") == std::string::npos;
        if (!sha_ok) firmware_sha.clear();
        const std::string saved_sha = savedFirmwareMark("sha");
        const std::string saved_url = savedFirmwareMark("url");
        const bool sha_hit = sha_ok && saved_sha == firmware_sha;
        const bool url_hit = !sha_ok && saved_url == firmware_url;
        const bool already = !firmware_url.empty() && (sha_hit || url_hit);
        const char *why = "empty";
        if (!firmware_url.empty() && sha_ok && saved_sha.empty()) why = "nomark";
        else if (!firmware_url.empty() && sha_ok) why = "sha";
        else if (!firmware_url.empty() && saved_url.empty()) why = "nomark";
        else if (!firmware_url.empty()) why = "url";
        std::string task_status = "-";
        if (!model_.tasks.empty()) {
            task_status = model_.tasks.front().status;
            if (task_status.empty() || task_status.size() > 32 ||
                task_status.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
                task_status = "other";
            }
        }
        firmware_trace_why_ = why;
        firmware_trace_task_ = task_status;
        firmware_trace_tasks_ = static_cast<int>(model_.tasks.size());
        firmware_trace_size_ = firmware_size;
        // 配置只记下地址。下载要等设置页里点「下载更新」。
        if (!firmware_update_requested_.load()) {
            firmware_url_ = firmware_url;
            firmware_sha_ = firmware_sha;
            firmware_version_ = firmware_version;
            firmware_size_ = firmware_size;
        }
        const bool pending = !firmware_url_.empty() && !already;
        firmware_available_.store(pending);
        if (!firmware_update_requested_.load() && !(firmware_trace_stage_ == "fail" && pending)) {
            firmware_trace_stage_ = firmware_url_.empty() ? "idle" : (pending ? "ready" : "skip");
            firmware_trace_result_ = firmware_url_.empty() ? "none" : (pending ? "wait" : "match");
            firmware_trace_written_ = -1;
            firmware_trace_http_ = 0;
            if (firmware_url_.empty()) firmware_status_ = T("电脑上还没有配置固件地址");
            else if (!pending) firmware_status_ = T("已是当前固件");
            else if (firmware_version_.empty()) firmware_status_ = T("有可下载的固件");
            else {
                std::string shown = firmware_version_;
                if (shown.size() > 32) shown.resize(32);
                firmware_status_ = std::string(T("有可下载的固件")) + " " + shown;
            }
            const std::string mark = firmware_trace_stage_ + why + ":" + std::to_string(firmware_size_);
            static std::string logged_mark;
            if (mark != logged_mark) {
                logged_mark = mark;
                ESP_LOGI("vibe_fw", "[fw] %s why=%s tasks=%d task=%s size=%d savedSha=%.12s offerSha=%.12s url=%.160s",
                    firmware_trace_stage_.c_str(), why, firmware_trace_tasks_, task_status.c_str(),
                    firmware_size_, saved_sha.c_str(), firmware_sha_.c_str(), firmware_url_.c_str());
            }
        }
        if (can_voice) model_.voice_hint.clear();
        voice_available_.store(can_voice);
        // 桥接器重启会丢掉命令清单：config 里带 announce 标志时重新上报。
        if (announce_catalog) board_catalog_sent_ = false;
        config_loaded_.store(true);
        ++model_.revision;
    }
}

// 行动系统（LVGL 线程，maybeApplyArtwork 每帧调用）：读当前素材舞台的
// 可用动作名单，与已上报的比对，变了就置 board_catalog_sent_ 为假等 worker
// 重报整张清单——动作作为命令条目（action.<名>）并入清单，不单开通道。
// 名单的真源是素材包 manifest：声明了 commands 表的素材上报命令号（A1–A9，
// 「命令绑素材」由素材作者定义），否则上报 actions 语义名（老素材兼容）。
void VibeCoding::syncAvailableActions()
{
    std::vector<std::string> names = made_sprite::actionNames(sprite_stage_);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        changed = names != available_actions_;
        if (changed) available_actions_ = std::move(names);
    }
    if (changed) {
        board_catalog_sent_ = false;   // 动作条目并入命令清单，变了整张重报
        std::string joined;
        for (const auto &name : names) {
            if (!joined.empty()) joined += ',';
            joined += name;
        }
        ESP_LOGI("vibe_actions", "[actions] list_changed count=%u list=%s",
            static_cast<unsigned>(names.size()), joined.c_str());
    }
}

void VibeCoding::queueBoardEvent(const char *name,
                                 const std::vector<std::pair<std::string, std::string>> &fields)
{
    if (!name || !name[0]) return;
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (std::find(board_events_.begin(), board_events_.end(), name) == board_events_.end()) return;
    if (!pending_board_event_.empty()) return;
    pending_board_event_ = name;
    pending_board_event_fields_ = fields;
}

// 摇一摇（vibe_motion 检测任务回调）：当前精灵页助手 = 选中的助手，切页
// 瞬间 selected_provider_ 就已更新，不等心跳。把助手 ID 塞进事件字段，
// 让桥接器插件按助手范围决定要不要下发动作。
void VibeCoding::notifyMotionShake()
{
    queueAssistantScopedEvent("motion.shake");
}

// 助手范围事件的公共装填：把当前精灵页助手 ID 塞进 fields 再入队（订阅
// 门禁在 queueBoardEvent 内部，没有插件订阅该事件时这里安静返回）。
void VibeCoding::queueAssistantScopedEvent(const char *name)
{
    std::vector<std::pair<std::string, std::string>> fields;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        std::string agent = selectedProviderLocked().id;
        if (vibe_provider::validId(agent)) fields.emplace_back("agent", std::move(agent));
    }
    queueBoardEvent(name, fields);
}

// 助手页双击（屏幕连续两次短按）：本板 I2C 总线上无 IMU（2026-10-18
// 全总线扫描只有 codec/touch，摇一摇硬件上不可实现），双击是插件侧
// shake-fall 的等价触发源。只在首页正常态生效；点在按钮/抽屉/弹窗等
// 子对象上的 SHORT_CLICKED 不会冒泡到 screen，天然不误触。
void VibeCoding::homeShortClickCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_code(event) != LV_EVENT_SHORT_CLICKED) return;
    if (!self->canOperate() || self->settings_open_.load() ||
        self->drawer_open_.load() || self->delete_dialog_open_.load())
        return;
    lv_obj_t *screen = lv_scr_act();
    // 只在首页（精灵页）：拖动中或拖动刚结束的松手都不算点击。
    if (self->home_dragging_ || lv_obj_get_x(screen) != 0) return;
    const uint32_t now = lv_tick_get();
    if (self->last_home_tap_ms_ != 0 && now - self->last_home_tap_ms_ <= 450) {
        self->last_home_tap_ms_ = 0;
        self->queueAssistantScopedEvent("touch.double");
    } else {
        self->last_home_tap_ms_ = now;
    }
}

void VibeCoding::announceBoardCatalog()
{
    // 动作作为命令条目并入清单：每个可用条目一条 action.<名>。设备自己决定
    // 支持哪些（两级绑定模型，docs/SPRITES.md）：素材 manifest 声明了
    // commands 表的，上报抽象命令号（A1–A9，事件绑定只发命令号）；没声明
    // 的老素材上报动作语义名（弃用中，向后兼容）。名单变化时调用方会重新
    // 上报整张清单。只输出符合桥接器 CAPABILITY_NAME 的名字（字母开头的
    // [A-Za-z0-9_]{0,15}），不合规的静默跳过，避免整张清单被拒。
    std::vector<std::string> actions;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        actions = available_actions_;
    }
    static constexpr const char *head =
        "{\"events\":["
        "{\"name\":\"boot.click\",\"cadence\":\"edge\"},"
        "{\"name\":\"boot.double\",\"cadence\":\"edge\"},"
        "{\"name\":\"boot.triple\",\"cadence\":\"edge\"},"
        "{\"name\":\"motion.shake\",\"cadence\":\"edge\"},"
        "{\"name\":\"touch.double\",\"cadence\":\"edge\"}"
        "],\"commands\":["
        "{\"name\":\"caption.show\"},"
        "{\"name\":\"audio.play\"},"
        "{\"name\":\"theme.apply\"},"
        "{\"name\":\"volume.set\"},"
        "{\"name\":\"agent.select\"},"
        "{\"name\":\"session.create\"},"
        "{\"name\":\"session.select\"},"
        "{\"name\":\"session.delete\"},"
        "{\"name\":\"voice.start\"},"
        "{\"name\":\"task.confirm\"},"
        "{\"name\":\"task.cancel\"},"
        "{\"name\":\"debug.screenshot\"},"
        "{\"name\":\"debug.heap\"},"
        "{\"name\":\"ui.swipe\"},"
        "{\"name\":\"ui.tap\"},"
        "{\"name\":\"ui.back\"}";
    std::string body = head;
    auto valid_action = [](const std::string &name) {
        if (name.empty() || name.size() > 16) return false;
        // 抽象命令号（A1–A9）或老语义名（小写字母开头 [a-z0-9_]{0,15}）
        const bool slot = name.size() == 2 && name[0] == 'A' && name[1] >= '1' && name[1] <= '9';
        if (!slot && (name[0] < 'a' || name[0] > 'z')) return false;
        for (const char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_';
            if (!ok) return false;
        }
        return true;
    };
    for (const auto &name : actions) {
        if (!valid_action(name)) continue;
        body += ",{\"name\":\"action.";
        body += name;
        body += "\"}";
    }
    body += "]}";
    std::string response;
    int status = 0;
    if (!request("/device/capabilities", true, response, status, body) || status != 200) return;
    board_catalog_sent_ = true;
    announced_actions_ = std::move(actions);   // worker only
}

void VibeCoding::flushBoardEvent()
{
    std::string body;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        // burn-in 观察通道优先：绕过订阅门禁直发（桥接器打日志后拒收未订阅
        // 事件，无副作用）。观察期没有别的插件事件源，挤占无害。
        if (!burnin_event_body_.empty()) {
            body = std::move(burnin_event_body_);
            burnin_event_body_.clear();
        }
    }
    if (body.empty()) {
        std::string name;
        std::vector<std::pair<std::string, std::string>> fields;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (pending_board_event_.empty()) return;
            if (std::find(board_events_.begin(), board_events_.end(), pending_board_event_) == board_events_.end()) {
                pending_board_event_.clear();
                pending_board_event_fields_.clear();
                return;
            }
            name = std::move(pending_board_event_);
            pending_board_event_.clear();
            fields = std::move(pending_board_event_fields_);
            pending_board_event_fields_.clear();
        }
        body = std::string("{\"name\":\"") + name + "\"";
        if (!fields.empty()) {
            body += ",\"fields\":{";
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i) body += ',';
                body += '"'; body += fields[i].first; body += "\":\"";
                for (const char c : fields[i].second) {   // JSON 字符串转义（保守）
                    if (c == '"' || c == '\\') body += '\\';
                    body += c;
                }
                body += '"';
            }
            body += '}';
        }
        body += '}';
    }
    std::string response;
    int status = 0;
    (void)request("/device/events", true, response, status, body);
}

namespace {
std::string trimNotice(std::string caption)
{
    if (caption.size() > 120) caption.resize(120);
    while (!caption.empty() && (static_cast<unsigned char>(caption.back()) & 0xC0) == 0x80) caption.pop_back();
    if (!caption.empty() && (static_cast<unsigned char>(caption.back()) & 0xC0) == 0xC0) caption.pop_back();
    return caption;
}

std::string commandField(const cJSON *command, const char *key)
{
    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(command, "fields");
    return cJSON_IsObject(fields) ? jsonString(fields, key) : std::string();
}

int parseLevel(const std::string &level)
{
    if (level.empty() || level.size() > 3) return -1;
    if (level.size() > 1 && level[0] == '0') return -1;
    int value = 0;
    for (char c : level) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    return value <= 100 ? value : -1;
}

bool validThemeName(const std::string &name)
{
    if (name == "default") return true;
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool validDirection(const std::string &direction)
{
    return direction == "next" || direction == "prev" ||
           direction == "left" || direction == "right" ||
           direction == "up" || direction == "down";
}

// ui.tap 命中测试：返回 (x,y) 处最深的可见可点击对象（顶层子对象优先），无则空。
lv_obj_t *findClickableAt(lv_obj_t *obj, const lv_point_t &point)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || !lv_obj_is_visible(obj)) return nullptr;
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    if (point.x < coords.x1 || point.x > coords.x2 || point.y < coords.y1 || point.y > coords.y2) return nullptr;
    const int32_t count = lv_obj_get_child_count(obj);
    for (int32_t i = count - 1; i >= 0; --i) {
        lv_obj_t *hit = findClickableAt(lv_obj_get_child(obj, i), point);
        if (hit) return hit;
    }
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? obj : nullptr;
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

// 调试用截屏（debug.screenshot 命令）：LVGL 线程内 lv_snapshot 渲染当前画面，
// 拷贝到 PSRAM 后开独立任务以 RGB565 裸帧 POST /device/screenshot，桥接器转 PNG 落盘。
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
    const lv_color_format_t format = LV_COLOR_FORMAT_RGB565;
    lv_draw_buf_t *buffer = lv_snapshot_create_draw_buf(screen, format);
    if (!buffer || lv_snapshot_take_to_draw_buf(screen, format, buffer) != LV_RESULT_OK) {
        if (buffer) lv_draw_buf_destroy(buffer);
        screenshot_busy_.store(false);
        return;
    }
    const int width = buffer->header.w;
    const int height = buffer->header.h;
    const uint32_t stride = buffer->header.stride;
    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(height);
    uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) copy = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    if (!copy) {
        lv_draw_buf_destroy(buffer);
        screenshot_busy_.store(false);
        return;
    }
    memcpy(copy, buffer->data, bytes);
    lv_draw_buf_destroy(buffer);
    struct ShotArgs {
        VibeCoding *self;
        uint8_t *data;
        size_t bytes;
        int width;
        int height;
        uint32_t stride;
    };
    if (xTaskCreate([](void *arg) {
            auto *args = static_cast<ShotArgs *>(arg);
            char path[96];
            snprintf(path, sizeof(path), "/device/screenshot?fmt=rgb565&w=%d&h=%d&stride=%lu",
                     args->width, args->height, static_cast<unsigned long>(args->stride));
            const std::string body(reinterpret_cast<const char *>(args->data), args->bytes);
            free(args->data);
            std::string response;
            int status = 0;
            (void)args->self->request(path, true, response, status, body, 4096, 20000);
            args->self->screenshot_busy_.store(false);
            delete args;
            vTaskDelete(nullptr);
        }, "vibe_shot", 16384, new ShotArgs{this, copy, bytes, width, height, stride}, 3, nullptr) != pdPASS) {
        free(copy);
        screenshot_busy_.store(false);
    }
}

// ui.swipe：语义化手势，动作与真实手势一致（left/right 切助手，up 开抽屉，down 收抽屉）。
void VibeCoding::handleUiSwipe(const std::string &direction)
{
    swipe_calls_.fetch_add(1);
    if (!canOperate() || settings_open_.load()) return;
    if (direction == "left") {
        // 抽屉（任务结果页）里横向切当前助手的不同任务，主屏切助手。
        drawer_open_.load() ? chooseSession(1) : chooseProvider(1);
    } else if (direction == "right") {
        drawer_open_.load() ? chooseSession(-1) : chooseProvider(-1);
    } else if (direction == "up") {
        if (!drawer_open_.load()) showDrawer(true);
    } else if (direction == "down") {
        if (drawer_open_.load()) showDrawer(false);
    }
}

// ui.tap：命中 (x,y) 处最深可见可点击对象并派发 CLICKED（本应用全部点击回调都挂该事件）。
// 调试/插件自动化用途；不做按下-抬起全流程模拟。
void VibeCoding::handleUiTap(const std::string &x_text, const std::string &y_text)
{
    if (!canOperate() || settings_open_.load()) return;
    if (x_text.empty() || x_text.size() > 4 || y_text.empty() || y_text.size() > 4) return;
    for (const char c : x_text + y_text) {
        if (c < '0' || c > '9') return;
    }
    const int x = atoi(x_text.c_str());
    const int y = atoi(y_text.c_str());
    lv_obj_t *screen = lv_screen_active();
    if (!screen || x < 0 || y < 0) return;
    const lv_point_t point{x, y};
    lv_obj_t *target = findClickableAt(screen, point);
    if (target) lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);
}

void VibeCoding::applyRemoteCommands()
{
    std::vector<RemoteCommand> batch;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        batch.swap(inbound_commands_);
    }
    cmd_calls_.fetch_add(batch.size());
    for (auto &command : batch) {
        if (command.name == "caption.show") {
            std::string caption = trimNotice(command.text);
            if (caption.empty()) continue;
            std::lock_guard<std::mutex> lock(model_mutex_);
            model_.session_notice = std::move(caption);
            ++model_.revision;
        } else if (command.name == "audio.play") {
            if (!command.audio.empty() &&
                !made_chime_play_wav(reinterpret_cast<const uint8_t *>(command.audio.data()), command.audio.size())) {
                std::lock_guard<std::mutex> lock(model_mutex_);
                model_.session_notice = T("无法播放下发的语音");
                ++model_.revision;
            }
        } else if (command.name == "theme.apply") {
            if (validThemeName(command.theme)) startThemeApply(command.theme);
        } else if (command.name == "volume.set") {
            const int level = parseLevel(command.level);
            if (level >= 0) setVolumeLevel(level);
        } else if (command.name == "agent.select") {
            if (vibe_provider::validId(command.target)) selectProviderById(command.target);
            else if (command.direction == "next") chooseProvider(1);
            else if (command.direction == "prev") chooseProvider(-1);
        } else if (command.name == "session.create") {
            requestNewSession();
        } else if (command.name == "session.select") {
            if (validSessionId(command.target)) selectSessionById(command.target);
            else if (command.direction == "next") chooseSession(1);
            else if (command.direction == "prev") chooseSession(-1);
        } else if (command.name == "session.delete") {
            if (validSessionId(command.target)) queueDeleteSessionId(command.target);
        } else if (command.name == "voice.start") {
            startVoice();
        } else if (command.name == "task.confirm" || command.name == "task.cancel") {
            std::string task_id = command.target;
            if (task_id.empty()) {
                std::lock_guard<std::mutex> lock(model_mutex_);
                task_id = visible_task_id_;
            }
            if (validTaskId(task_id)) {
                queueActionForId(command.name == "task.confirm" ? "confirm" : "cancel", task_id);
            }
        } else if (command.name == "debug.screenshot") {
            startScreenshotUpload();
        } else if (command.name == "debug.heap") {
            logHeapDiagnostics();
        } else if (command.name == "action.play" || command.name.rfind("action.", 0) == 0) {
            // 行动系统：动作即命令（action.<名>，或兼容旧 action.play+fields.action）。
            // 两级绑定模型：命令名通常是抽象命令号（action.A1…），playAction
            // 按 manifest 的 commands 表映射到真实动作；素材没映射就静默
            // 忽略（降级不播，不报错）。
            std::string action = command.action;
            if (command.name.size() > 7) action = command.name.substr(7);
            if (!action.empty() && action.size() <= 16)
                made_sprite::playAction(sprite_stage_, action.c_str());
        } else if (command.name == "ui.swipe") {
            handleUiSwipe(command.direction);
        } else if (command.name == "ui.tap") {
            handleUiTap(command.x, command.y);
        } else if (command.name == "ui.back") {
            // 远程返回键：与实体返回键同一条 back() 路径（设置面板逐层退出）。
            // 故意不走 settings_open_ 拦截，否则设置页打开时远程无法恢复。
            back();
        }
    }
}

void VibeCoding::pullBoardCommands()
{
    auto postAck = [this](const std::vector<std::string> &ids) {
        if (ids.empty()) return true;
        std::string body = "{\"ids\":[";
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) body += ',';
            body += '"';
            body += ids[i];
            body += '"';
        }
        body += "]}";
        std::string response;
        int status = 0;
        return request("/device/commands/ack", true, response, status, body) && status == 200;
    };
    if (!ack_retry_.empty() && postAck(ack_retry_)) ack_retry_.clear();
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (inbound_commands_.size() >= 8) return;
    }
    std::string response;
    int status = 0;
    if (!request("/device/commands", false, response, status) || status != 200) return;
    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) return;
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
    std::vector<RemoteCommand> accepted;
    std::vector<std::string> ack_ids = ack_retry_;
    if (cJSON_IsArray(commands)) {
        int count = 0;
        cJSON *command = nullptr;
        cJSON_ArrayForEach(command, commands) {
            if (count++ >= 8) break;
            if (!cJSON_IsObject(command)) continue;
            const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(command, "id");
            const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(command, "name");
            if (!cJSON_IsString(id_item) || !validCommandId(id_item->valuestring) || !cJSON_IsString(name_item)) continue;
            const std::string id = id_item->valuestring;
            if (std::find(ack_ids.begin(), ack_ids.end(), id) == ack_ids.end()) ack_ids.push_back(id);
            if (std::find(delivered_command_ids_.begin(), delivered_command_ids_.end(), id) != delivered_command_ids_.end()) continue;
            RemoteCommand next;
            next.id = id;
            next.name = name_item->valuestring;
            next.text = commandField(command, "text");
            next.target = commandField(command, "id");
            next.direction = commandField(command, "direction");
            next.level = commandField(command, "level");
            next.theme = commandField(command, "name");
            next.x = commandField(command, "x");
            next.y = commandField(command, "y");
            next.action = commandField(command, "action");
            if (!validDirection(next.direction)) next.direction.clear();
            const cJSON *audio = cJSON_GetObjectItemCaseSensitive(command, "audio");
            if (cJSON_IsTrue(audio)) {
                std::string clip;
                int clip_status = 0;
                const std::string path = "/device/commands/" + id + "/audio";
                if (!request(path, false, clip, clip_status, "", 256 * 1024, 35000) || clip_status != 200 || clip.size() < 44) {
                    ack_ids.erase(std::remove(ack_ids.begin(), ack_ids.end(), id), ack_ids.end());
                    continue;
                }
                next.audio = std::move(clip);
            }
            delivered_command_ids_.push_back(id);
            if (delivered_command_ids_.size() > 32) delivered_command_ids_.erase(delivered_command_ids_.begin());
            accepted.push_back(std::move(next));
        }
    }
    cJSON_Delete(root);
    if (!accepted.empty()) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        for (auto &command : accepted) {
            if (inbound_commands_.size() >= 8) {
                delivered_command_ids_.erase(std::remove(delivered_command_ids_.begin(), delivered_command_ids_.end(), command.id),
                                             delivered_command_ids_.end());
                ack_ids.erase(std::remove(ack_ids.begin(), ack_ids.end(), command.id), ack_ids.end());
                continue;
            }
            inbound_commands_.push_back(std::move(command));
        }
    }
    if (!postAck(ack_ids)) ack_retry_ = std::move(ack_ids);
    else ack_retry_.clear();
}

} // namespace esp_brookesia::apps
