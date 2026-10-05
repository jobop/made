#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// 可插拔主题：主题包放桥接器 themes/<名字>/ 目录（theme.json + 可选资产），
// 设备经 HTTP 下载到 SPIFFS /spiffs/themes/<名字>/，设置里选中生效。
// 默认主题即当前内置界面；应用主题后重启生效。

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
    bool has_icon = false;   // raw RGB565（与 LVGL 字节序一致）
    bool has_bg = false;
    uint8_t *icon_data = nullptr;
    uint16_t icon_w = 0, icon_h = 0;
    uint8_t *bg_data = nullptr;
    uint16_t bg_w = 0, bg_h = 0;
};

struct ThemeInfo {
    std::string name;   // 目录名
    std::string title;  // 显示名（theme.json 的 title，缺省用目录名）
};

// 启动后调用一次：读 NVS 激活主题并加载调色板/锁屏资产。未激活 = 默认。
esp_err_t init();
const Palette &palette();
const LockAsset &lockAsset();
std::string activeName();  // "" = 默认主题

// SPIFFS 上已安装的主题（不含内置默认）。
std::vector<ThemeInfo> list();
// 应用主题（name 为 "" 恢复默认）。写入 NVS 并加载资产；UI 需重启后生效。
esp_err_t apply(const std::string &name);
esp_err_t remove(const std::string &name);

// 从桥接器同步主题（阻塞网络操作，请在独立任务调用）：
// GET <base>/api/themes；GET <base>/api/themes/<name>；GET .../files/<file>
esp_err_t syncFromBridge(const std::string &base_url);

// USB 直连模式的同步：请求走 vibe_usb 串口帧（GET /api/themes…），其余同上。
esp_err_t syncOverUsb();

enum class SyncState { Idle, Running, Done, Failed };
struct SyncStatus {
    SyncState state = SyncState::Idle;
    std::string message;
};
SyncStatus syncStatus();  // 线程安全

// 主题包内文件写入（同步流程内部使用；也可手动喂文件）。
esp_err_t saveFile(const std::string &theme, const std::string &filename,
                   const uint8_t *data, size_t size);

} // namespace vibe_theme
