#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// 主题系统：主题包只存在于桥接器（themes/<名字>/theme.json + 可选资产）。
// 板端只缓存"当前生效"主题：调色板存 NVS，锁屏资产存 /spiffs 扁平文件
// （theme_icon.bin / theme_bg.bin，SPIFFS 无目录概念，刻意避开子目录）。
// 主题列表不落盘——打开设置时通过当前连接实时拉取。
// 应用主题后重启生效；默认主题（未激活）即内置界面。

namespace vibe_theme {

struct Palette {
    uint32_t accent;         // 高亮/选中按钮
    uint32_t btn_normal;     // 普通按钮
    uint32_t btn_cancel;     // 取消按钮
    uint32_t border;         // 按钮描边
    uint32_t text_primary;   // 主文字
    uint32_t text_secondary; // 次要文字
    uint32_t text_hint;      // 提示文字
    uint32_t text_accent;    // 成功/强调文字
    uint32_t title;          // 页面标题
    uint32_t bg_page;        // 页面背景
    uint32_t bg_panel;       // 面板背景
    uint32_t bg_row;         // 列表行背景
    uint32_t danger;         // 危险/失败
    uint32_t lock_bg;        // 锁屏背景
};

struct LockAsset {
    bool has_icon = false;   // raw RGB565 小端（与固件字节序一致）
    bool has_bg = false;
    uint8_t *icon_data = nullptr;
    uint16_t icon_w = 0, icon_h = 0;
    uint8_t *bg_data = nullptr;
    uint16_t bg_w = 0, bg_h = 0;
};

struct ThemeInfo {
    std::string name;   // 目录名
    std::string title;  // 显示名
};

// 启动后调用一次：从 NVS 恢复调色板与锁屏资产缓存。
esp_err_t init();
const Palette &palette();
const LockAsset &lockAsset();
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
