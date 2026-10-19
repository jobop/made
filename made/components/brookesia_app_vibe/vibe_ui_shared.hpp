#pragma once
// esp_brookesia_app_vibe 拆分后的组件内共享工具层。
// 原先全部挤在 esp_brookesia_app_vibe.cpp 的匿名空间里，拆分出
// vibe_settings_pages / vibe_artwork / vibe_backend 等编译单元后，
// 这些跨单元使用的辅助函数/常量/状态收口到这里。
// 仅限本组件内部使用，不是公共 API。
#include <cstdint>
#include <string>
#include <vector>

#include "cJSON.h"
#include "lvgl.h"
#include "vibe_i18n.hpp"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_4);

namespace esp_brookesia::apps {

// —— 常量 ——
inline constexpr size_t MAX_RESPONSE_BYTES = 32 * 1024;
inline constexpr uint32_t POLL_INTERVAL_MS = 2200;
inline constexpr uint64_t HEARTBEAT_INTERVAL_MS = 12000;
// 公网隧道（HTTPS）的一次往返远慢于局域网：握手本身常常就要数秒，
// 默认 5 秒预算会被握手吃光。局域网/USB 仍保留调用方给的短超时。
inline constexpr int REMOTE_REQUEST_TIMEOUT_MS = 15000;
inline constexpr char BUTTON_TAG[] = "vibe_boot";

// —— 主题快捷宏（vibe_theme::palette().<字段>）——
#define VT(key) (vibe_theme::palette().key)

// —— 字体（主题无关：正文 16px，抽屉 14px；字库 glyph 在 flash 不占 PSRAM）——
extern const lv_font_t *text_font;
extern const lv_font_t *drawer_text_font;

// —— i18n ——
inline const char *T(const char *key) { return vibe_i18n::tr(key); }

struct LocalizedLabel {
    lv_obj_t *object;
    std::string key;
};
// LVGL 线程专用：本应用屏幕上的全部本地化标签。
extern std::vector<LocalizedLabel> localized_labels;

void bindLabel(lv_obj_t *label, const char *key);

// —— 接收器/USB 直连状态 ——
bool receiverVoiceUploading();

// —— 布局/样式工具 ——
int made_radius(int design_radius);
void markPage(lv_obj_t *object);
bool isPage(lv_obj_t *object);
void placePageHeader(lv_obj_t *left, lv_obj_t *title, lv_obj_t *right);
uint32_t mixColor(uint32_t a, uint32_t b, uint32_t weight_a, uint32_t weight_b = 0);
void styleSurface(lv_obj_t *button, uint32_t color);

// —— 通用小部件（页面/弹层共用）——
lv_obj_t *touchButton(lv_obj_t *parent, int x, int y, int width, int height,
                      const char *caption, lv_event_cb_t callback, void *user,
                      uint32_t color = 0);
lv_obj_t *settingsLabel(lv_obj_t *parent, const char *caption, int x, int y, int width,
                        uint32_t color = 0, bool localize = true);

// —— 触摸工具（滑动起点跟踪，防误触）——
void trackTap(lv_event_t *event);
void ignoreSwipeClick(lv_event_t *event);

// —— 解析/校验 ——
std::string jsonString(const cJSON *object, const char *key);
bool validTaskId(const std::string &id);
bool validProjectId(const std::string &id);
bool validCommandId(const char *id);
bool validSessionId(const std::string &id);
const char *statusName(const std::string &status);

// —— 固件标记（NVS vibe_fw 命名空间）——
std::string savedFirmwareMark(const char *key);
void rememberFirmwareMark(const char *key, const std::string &value);

} // namespace esp_brookesia::apps
