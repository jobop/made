#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// 主题系统：主题包只存在于桥接器（themes/<名字>/theme.json + 可选资产）。
// 板端只缓存"当前生效"主题：调色板与风格参数存 NVS。
// 主题列表不落盘——打开设置时通过当前连接实时拉取。
// 应用主题后重启生效；默认主题（未激活）即内置界面（浅色清新）。
//
// theme.json 可控能力：
//   colors.{...}          调色板（下方 Palette 全部键，缺省回落默认值）
//   base: "light"|"dark"  亮暗元数据：固件据此适配阴影/残色等细节
//   metrics.radius_scale  圆角缩放百分比（50~200）
//   shape: "rounded"|"circle"|"square"  形状语言（按钮/chip 圆角风格）
//   font: 16|20           中文点阵字号档。固件只内置 16px，写 20 时按 16px 显示
//   sound.file            提示音 WAV（≤256KB）

namespace vibe_theme {

// 形状语言取值（shapeMode() 返回）
enum ShapeMode { kShapeRounded = 0, kShapeCircle = 1, kShapeSquare = 2 };

struct Palette {
    uint32_t accent;         // 高亮/选中按钮
    uint32_t btn_normal;     // 普通按钮
    uint32_t btn_cancel;     // 取消按钮
    uint32_t border;         // 按钮描边
    uint32_t text_primary;   // 主文字
    uint32_t text_secondary; // 次要文字
    uint32_t text_hint;      // 提示文字
    uint32_t text_accent;    // 成功/强调文字
    uint32_t text_answer;    // 任务回显正文（介于 secondary 与 hint 之间）
    uint32_t text_on_accent; // accent 实底上的文字/图标
    uint32_t title;          // 页面标题
    uint32_t bg_page;        // 页面背景
    uint32_t bg_panel;       // 面板背景
    uint32_t bg_row;         // 列表行背景
    uint32_t home_bg;        // 助手（精灵）舞台背景
    uint32_t danger;         // 危险/失败
    // —— 以下为 UI 重设计新增 token（主题包可覆盖；旧包缺 key 用默认值）——
    uint32_t chip_accent;    // 强调色浅底（状态徽章/图标位），accent 的浅色衍生
    uint32_t danger_soft;    // 危险弱化底（破坏性按钮，弱于 danger）
    uint32_t warning;        // 待确认/排队/工作状态点
    uint32_t success;        // 成功/在线状态点
};

struct ThemeInfo {
    std::string name;   // 目录名
    std::string title;  // 显示名
};

// 启动后调用一次：从 NVS 恢复调色板与风格参数。
esp_err_t init();
const Palette &palette();
int radiusScale();      // 50~200，默认 100
int shapeMode();        // ShapeMode，默认 kShapeRounded
bool isLightBase();     // base 亮暗元数据，默认 true（内置浅色）
int fontPx();           // 中文点阵字号，固定 16

// 主题提示音 WAV 路径；空串 = 使用内置提示音。
std::string soundPath();
std::string activeName();  // "" = 默认主题

// 实时主题列表（base_url 为空 = 走 USB 串口帧传输；否则走 HTTP）。
esp_err_t fetchThemeList(const std::string &base_url, std::vector<ThemeInfo> &out);

// 应用主题：经当前连接拉取数据并写缓存，重启后生效。name 为 "" 恢复默认。
esp_err_t applyFromBridge(const std::string &name, const std::string &base_url);

enum class SyncState { Idle, Running, Done, Failed };
struct SyncStatus {
    SyncState state = SyncState::Idle;
    std::string message;
};
SyncStatus syncStatus();  // 线程安全

} // namespace vibe_theme
