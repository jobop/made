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
#if defined(MADE_USE_FREETYPE)
#include <map>
#include <string>
#endif
#include "vibe_ui_shared.hpp"

namespace esp_brookesia::apps {

std::vector<LocalizedLabel> localized_labels; // LVGL thread only; all belong to this app screen.
void bindLabel(lv_obj_t *label, const char *key) {
    if (key && vibe_i18n::known(key)) localized_labels.push_back({label, key});
}


// 公网隧道（HTTPS）的一次往返远慢于局域网：握手本身常常就要数秒，
// 默认 5 秒预算会被握手吃光。局域网/USB 仍保留调用方给的短超时。

bool receiverVoiceUploading()
{
    const auto mode = vibe_pairing::snapshot().access_mode;
    return (mode == vibe_pairing::AccessMode::Receiver || mode == vibe_pairing::AccessMode::UsbDirect) &&
           vibe_voice::status().phase == vibe_voice::Phase::Uploading;
}

// The bundled Puhui font includes common Han characters missing from LVGL's
// small Source Han Sans subset. Its glyph data lives in flash, not PSRAM.
// 中文正文固定 16px。主题包 font 写 20 时也按 16px 显示。
const lv_font_t *text_font = &font_puhui_16_4;
// 抽屉任务区比正文小一号，用 14px。
// 14px 必须用 4bpp 版（fonts/font_puhui_14_4.c 由 ttf/puhui-common.ttf 生成）：
// 字库组件自带的 font_puhui_14_1 是 1bpp 单色版，浅色底上无抗锯齿会发糊。
const lv_font_t *drawer_text_font = &font_puhui_14_4;

} // namespace esp_brookesia::apps

// ---------------- 运行时字体适配（控件大小随屏幕等比缩放）----------------
// 声明在全局头 made_layout.hpp，实现也必须位于全局命名空间（各页面按
// made_font(...) / made_icon_font(...) 直接调用）。
#if defined(MADE_USE_FREETYPE)
namespace {
std::map<int, lv_font_t *> g_made_text_fonts;
std::map<int, lv_font_t *> g_made_icon_fonts;
bool g_freetype_ready = false;
// TTF 所在目录，运行时可设置（安卓把 APK assets 里的 TTF 释放到内部存储后
// 传入真实路径）；桌面 sim 不设置时回落到编译期 MADE_TTF_DIR。
std::string g_made_ttf_dir;

// 360 设计字号 → 当前屏幕物理字号（k=短边/360，四舍五入）。
int made_phys_px(int design)
{
    const int px = (design * made_fit() + made_design / 2) / made_design;
    return px > 0 ? px : design;
}
std::string made_ttf_path(const char *name)
{
    std::string dir = g_made_ttf_dir;
#ifdef MADE_TTF_DIR
    if (dir.empty()) dir = MADE_TTF_DIR;
#endif
    if (dir.empty()) return std::string(name);
    return dir + "/" + name;
}
} // namespace

void made_fonts_set_dir(const char *dir)
{
    if (dir && *dir) g_made_ttf_dir = dir;
}

void made_fonts_init()
{
    if (g_freetype_ready) return;
    lv_freetype_init(1024);
    g_freetype_ready = true;
}

// 正文/数字/中文用普惠矢量；混排的 LV_SYMBOL 走同号 FontAwesome fallback。
const lv_font_t *made_font(int design)
{
    auto it = g_made_text_fonts.find(design);
    if (it != g_made_text_fonts.end()) return it->second;
    lv_font_t *f = lv_freetype_font_create(
        made_ttf_path("puhui-common.ttf").c_str(),
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, made_phys_px(design),
        LV_FREETYPE_FONT_STYLE_NORMAL);
    if (f) {
        // 同号 FontAwesome 矢量作为图标 fallback：纯图标 label 普惠查不到
        // 字形时自动落到这里，数字/中文照常走普惠，一个字体覆盖全部。
        auto ic = g_made_icon_fonts.find(design);
        lv_font_t *icon;
        if (ic != g_made_icon_fonts.end()) {
            icon = ic->second;
        } else {
            icon = lv_freetype_font_create(
                made_ttf_path("fontawesome.ttf").c_str(),
                LV_FREETYPE_FONT_RENDER_MODE_BITMAP, made_phys_px(design),
                LV_FREETYPE_FONT_STYLE_NORMAL);
            if (!icon) icon = const_cast<lv_font_t *>(LV_FONT_DEFAULT);
            g_made_icon_fonts[design] = icon;
        }
        f->fallback = icon;
    } else {
        f = const_cast<lv_font_t *>(LV_FONT_DEFAULT);
    }
    g_made_text_fonts[design] = f;
    return f;
}

// 图标/数字/页码统一走万能字体（普惠+图标 fallback）；纯图标自动落到
// FontAwesome，数字/名称走普惠。
const lv_font_t *made_icon_font(int design)
{
    return made_font(design);
}
#else
// 真机固件：点阵字 k=1，1:1 最清晰，保持原视觉零回归。
void made_fonts_init() {}

const lv_font_t *made_icon_font(int design)
{
    if (design >= 24) return &lv_font_montserrat_26;
    if (design >= 19) return &lv_font_montserrat_20;
    if (design >= 15) return &lv_font_montserrat_16;
    if (design >= 13) return &lv_font_montserrat_14;
    return &lv_font_montserrat_12;
}

const lv_font_t *made_font(int design)
{
    if (design >= 15) return &font_puhui_16_4;
    return &font_puhui_14_4;
}
#endif

namespace esp_brookesia::apps {

// 主题风格半径：made_s(设计像素) 后再乘主题包的圆角缩放。
int made_radius(int design_radius)
{
    const int scaled = made_s(design_radius) * vibe_theme::radiusScale() / 100;
    return scaled > 0 ? scaled : 1;
}

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

lv_point_t tap_origin{};
bool tap_tracking = false;

void trackTap(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) return;
    lv_indev_t *indev = lv_indev_active();
    tap_tracking = false;
    if (!indev) return;
    lv_indev_get_point(indev, &tap_origin);
    tap_tracking = true;
}

// A swipe that starts on a button must not activate it. Registered ahead of
// the click callback so stop_processing drops that click.
void ignoreSwipeClick(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !tap_tracking) return;
    lv_point_t now{};
    lv_indev_get_point(indev, &now);
    tap_tracking = false;
    if (std::abs(now.x - tap_origin.x) > 12 || std::abs(now.y - tap_origin.y) > 12 ||
        lv_indev_get_gesture_dir(indev) != LV_DIR_NONE)
        lv_event_stop_processing(event);
}

// 线性混合两色（整型权重），用于推导按钮的高光/暗部。
uint32_t mixColor(uint32_t a, uint32_t b, uint32_t weight_a, uint32_t weight_b)
{
    const uint32_t sum = weight_a + weight_b ? weight_a + weight_b : 1;
    const uint32_t r = (((a >> 16) & 0xFF) * weight_a + ((b >> 16) & 0xFF) * weight_b) / sum;
    const uint32_t g = (((a >> 8) & 0xFF) * weight_a + ((b >> 8) & 0xFF) * weight_b) / sum;
    const uint32_t bl = ((a & 0xFF) * weight_a + (b & 0xFF) * weight_b) / sum;
    return (r << 16) | (g << 8) | bl;
}

// 按钮统一表面：上亮下暗的微渐变 + 柔和投影代替硬描边；按下整体提亮、
// 投影收紧并轻微下沉，150ms 过渡让反馈可感知而不突兀。
void styleSurface(lv_obj_t *button, uint32_t color)
{
    const uint32_t highlight = mixColor(color, 0xFFFFFF, 7, 1);
    // 浅色底上黑色投影会显得脏而沉重：换暖灰并大幅降低不透明度。
    const bool light = vibe_theme::isLightBase();
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_grad_color(button, lv_color_hex(mixColor(color, 0x000000, 15, 1)), 0);
    lv_obj_set_style_bg_grad_dir(button, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_color(button, lv_color_hex(light ? 0x51655B : 0x000000), 0);
    lv_obj_set_style_shadow_width(button, made_s(14), 0);
    lv_obj_set_style_shadow_spread(button, made_s(1), 0);
    lv_obj_set_style_shadow_opa(button, light ? LV_OPA_20 : LV_OPA_50, 0);
    lv_obj_set_style_shadow_offset_y(button, made_s(3), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(highlight), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, made_s(5), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(button, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(button, made_s(1), LV_STATE_PRESSED);
    static lv_style_prop_t surface_props[] = {
        LV_STYLE_BG_COLOR, LV_STYLE_BG_GRAD_COLOR, LV_STYLE_SHADOW_WIDTH,
        LV_STYLE_SHADOW_OPA, LV_STYLE_TRANSLATE_Y,
    };
    static lv_style_transition_dsc_t surface_transition;
    lv_style_transition_dsc_init(&surface_transition, surface_props,
                                 lv_anim_path_ease_out, 150, 0, nullptr);
    lv_obj_set_style_transition(button, &surface_transition, 0);
    lv_obj_set_style_transition(button, &surface_transition, LV_STATE_PRESSED);
}

lv_obj_t *touchButton(lv_obj_t *parent, int x, int y, int width, int height,
                      const char *caption, lv_event_cb_t callback, void *user,
                      uint32_t color)
{
    const bool page = isPage(parent);
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, page ? made_page_x(x) : made_x(x), page ? made_page_y(y) : made_y(y));
    lv_obj_set_size(button, page ? made_page_w(width) : made_s(width), page ? made_page_h(height) : made_s(height));
    // 形状语言：square=2px 直角，circle=胶囊，rounded=主题缩放的圆角。
    const int shape = vibe_theme::shapeMode();
    const int radius = shape == vibe_theme::kShapeSquare ? 2 :
        shape == vibe_theme::kShapeCircle ? made_s(width < height ? width : height) / 2 :
        made_radius(12);
    lv_obj_set_style_radius(button, radius, 0);
    lv_obj_set_overflow_visible(button, true);
    if (color == 0) {
        // Ghost 次级按钮（Air 语言默认）：无底色 + 1px 细描边，按下泛灰。
        // 去掉实底色块与投影，界面立刻轻下来；主操作才允许实底。
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(VT(border)), 0);
        lv_obj_set_style_border_width(button, made_s(1), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(VT(bg_row)), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(button, 0, 0);
    } else {
        styleSurface(button, color);
    }
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(button, trackTap, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(button, ignoreSwipeClick, LV_EVENT_CLICKED, nullptr);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user);
    if (caption) {
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, T(caption));
        bindLabel(label, caption);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(VT(text_primary)), 0);
        lv_obj_set_style_text_font(label, text_font, 0);
        made_text(label, width - 12, true);
        lv_obj_center(label);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return button;
}

lv_obj_t *settingsLabel(lv_obj_t *parent, const char *caption, int x, int y, int width,
                        uint32_t color, bool localize)
{
    lv_obj_t *label = lv_label_create(parent);
    if (color == 0) color = VT(title);
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

bool validCommandId(const char *id)
{
    if (!id || id[0] < '1' || id[0] > '9') return false;
    const size_t length = strnlen(id, 17);
    if (length > 16) return false;
    for (size_t i = 1; i < length; ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
    }
    return true;
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

std::string savedFirmwareMark(const char *key)
{
    nvs_handle_t handle;
    if (nvs_open("vibe_fw", NVS_READONLY, &handle) != ESP_OK) return {};
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0 || length > 200) {
        nvs_close(handle);
        return {};
    }
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) {
        nvs_close(handle);
        return {};
    }
    nvs_close(handle);
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

void rememberFirmwareMark(const char *key, const std::string &value)
{
    nvs_handle_t handle;
    if (nvs_open("vibe_fw", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_str(handle, key, value.c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

} // namespace esp_brookesia::apps
