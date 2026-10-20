// SPDX-License-Identifier: Apache-2.0
// UI 构建模块：run() 的全部界面搭建，按屏幕区域拆成独立的 build 函数——
// 每个函数只建一个区域（主舞台/设置/抽屉/浮层），可单独修改与验证。
// 顺序敏感点：delete_dialog_ 必须在 drawer_ 之后创建（保证弹窗层级更高）；
// 小屏(rect)重排依赖 answer 区已创建。行为与原 run() 单函数版完全一致。
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_ui_shared.hpp"

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "made_layout.hpp"
#include "made_sprite.hpp"
#include "status_icons.h"
#include "vibe_i18n.hpp"

namespace esp_brookesia::apps {

// run() 前置：浮层状态归零、screen 真翻页配置（overflow + ext_draw_size）、
// 手势/双击回调挂接。
void VibeCoding::prepareRunState()
{
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
}

// 主页（精灵页）：近全屏精灵 + 左右侧台 + 页码 + 新建/删除会话按钮。
void VibeCoding::buildHomeStage()
{
    lv_obj_t *screen = lv_scr_act();
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
}

// 设置主页：齿轮入口 + overlay 容器 + 分组行列表（连接/偏好/系统）。
void VibeCoding::buildSettingsHome()
{
    lv_obj_t *screen = lv_scr_act();
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
}

// 设置子面板（语言/主题/电源/固件/音量）：每面板满屏、默认隐藏。
void VibeCoding::buildSettingsPanels()
{
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
}

// 接入设置面板 + 接收端选择浮层。
void VibeCoding::buildAccessPanel()
{
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
}

// 网络设置、字段编辑器、手机配置三个浮层。
void VibeCoding::buildNetworkAndEditorPanels()
{
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
    (void)phone_title;
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
}

// 主页状态件：精灵名字、素材页状态图标、连接/未读角标、任务标题行。
void VibeCoding::buildStatusWidgets()
{
    lv_obj_t *screen = lv_scr_act();
    provider_label_ = lv_label_create(screen);
    lv_obj_set_pos(provider_label_, made_x(70), made_home_y(302));
    lv_obj_set_style_text_align(provider_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(provider_label_, lv_color_hex(VT(text_primary)), 0);
    lv_obj_set_style_text_font(provider_label_, &lv_font_montserrat_26, 0);
    made_text(provider_label_, 220, false);

    // 素材页顶部状态图标（单一位置换图标，设计稿 2026-10-19）：48×48
    // recolor 位图，透明背景（2026-10-20 去掉药丸底，直接叠在页面上）。
    // 抽屉打开时隐藏（抽屉右上已有文字徽章）。
    const int icon_pill_w = made_s(76);
    const int icon_pill_h = made_s(60);
    status_icon_pill_ = lv_obj_create(screen);
    lv_obj_remove_style_all(status_icon_pill_);
    lv_obj_set_size(status_icon_pill_, icon_pill_w, icon_pill_h);
    lv_obj_set_pos(status_icon_pill_, made_x(180) - icon_pill_w / 2, made_home_y(6));
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
}

// 主页回答区（配对码/任务状态/指令/回显）+ 矩形小屏的整页重排。
void VibeCoding::buildAnswerArea()
{
    lv_obj_t *screen = lv_scr_act();
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
}

// 电脑选择浮层（配对用）：大触控行列表 + 刷新。
void VibeCoding::buildBridgePicker()
{
    // A separate list gives each discovered computer a large touch target.
    // Selection only queues worker work; discovery and pairing never block LVGL.
    bridge_picker_ = lv_obj_create(lv_scr_act());
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
}

// 抽屉（任务/回显/会话收纳）+ 删除确认弹窗。顺序敏感：drawer_ 先建，
// delete_dialog_ 后建保证弹窗层级永远更高。
void VibeCoding::buildDrawer()
{
    lv_obj_t *screen = lv_scr_act();
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
}

} // namespace esp_brookesia::apps
