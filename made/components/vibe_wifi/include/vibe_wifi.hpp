#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "esp_err.h"

namespace vibe_wifi {

// Call once after nvs_flash_init(). It starts a STA without waiting for a
// connection and reuses any ESP-IDF Wi-Fi configuration already in NVS.
// STA 当前是否已连接（含拿到 IP）。用于功能在发请求前等网络就绪。
bool station_connected();

esp_err_t start();

// Optional entry point for a future Brookesia Settings screen. Credentials
// are stored in this app's NVS namespace and become active immediately.
esp_err_t save_credentials(const char* ssid, const char* password);

// The optional USB receiver advertises a private WPA2 hotspot. Its credentials
// live in separate NVS keys; using it temporarily changes only the Wi-Fi
// driver's RAM configuration, so the previous station credentials survive.
esp_err_t save_receiver_credentials(const char* ssid, const char* password);
bool receiver_ssid(char* out, size_t capacity);
esp_err_t connect_receiver();
esp_err_t restore_station();
bool receiver_active();
// True after the user joins a network from 网络设置, until they explicitly
// connect the receiver hotspot again.
bool station_held();

// 历史 API：过去在传输敏感期临时关闭 Wi-Fi modem 省电（MIN_MODEM 的
// listen interval 约 300ms，弱信号下 HTTP 建连与分片传输频繁超时）。
// 现省电已常驻关闭（esp_wifi_start 后即设 WIFI_PS_NONE）：省电模式空闲
// 被 AP 判不活跃踢下线（reason 4，约 40s 一次）是断线根因。本对函数
// 保留为空实现，WifiThroughput RAII guard 调用点无需改动。
void begin_high_throughput();
void end_high_throughput();

struct WifiThroughput {
    WifiThroughput() { begin_high_throughput(); }
    ~WifiThroughput() { end_high_throughput(); }
    WifiThroughput(const WifiThroughput&) = delete;
    WifiThroughput& operator=(const WifiThroughput&) = delete;
};
int disconnect_reason();
uint32_t disconnect_count();

// 诊断：区分「自己主动断开」（固件代码调用 esp_wifi_disconnect，如扫描/切网）
// 与「被动断开」（AP 踢/信号丢失）。原因码 8 (ASSOC_LEAVE) 两者都会报，
// 单靠 reason 分不清——这个计数器记录固件主动断开的次数，事件层比对差值归因。
uint32_t voluntary_disconnects();

// 最近几次断线的历史快照（带时间/原因/主动与否/RSSI/空闲堆），
// 供心跳 trace 上报。返回紧凑串："ms:reason:self:rssi:heap;..."（最多 6 条）。
std::string disconnect_history();

enum class ReceiverScanState { Idle, Scanning, Done, Failed };

struct ReceiverNetwork {
    std::string ssid;
    int rssi = 0;
};

struct ReceiverScanSnapshot {
    ReceiverScanState state = ReceiverScanState::Idle;
    esp_err_t error = ESP_OK;
    std::vector<ReceiverNetwork> networks;
    uint32_t generation = 0;
};

// Starts an asynchronous scan without changing saved credentials or switching
// networks. Existing connections stay up; connection retries pause during scan.
// Results contain up to 12 distinct VibeReceiver-* hotspots, strongest first.
esp_err_t request_receiver_scan();
ReceiverScanSnapshot receiver_scan_snapshot();

// Detaches the page from its scan. An in-flight scan finishes in the background
// so we can release its driver results without stopping another app's Wi-Fi.
void cancel_receiver_scan();

struct WifiApNetwork {
    std::string ssid;
    int rssi = 0;
    bool open = false;
};

struct StationScanSnapshot {
    ReceiverScanState state = ReceiverScanState::Idle;
    esp_err_t error = ESP_OK;
    std::vector<WifiApNetwork> networks;
    uint32_t generation = 0;
};

// Same ownership rules as the receiver scan, but keeps every visible AP
// (hidden SSIDs skipped), strongest first, capped at 20 entries.
esp_err_t request_station_scan();
StationScanSnapshot station_scan_snapshot();

struct SetupAccessPoint {
    std::string ssid;
    std::string password;
    std::string address;
};

// Worker-only lifecycle for the temporary phone configuration hotspot. The
// existing station remains connected; credentials and mode are changed in RAM
// only. Returns INVALID_STATE while scanning or when another app owns an AP.
// The address is an HTTP URL. Passwords are regenerated each time it is opened.
esp_err_t start_setup_ap(SetupAccessPoint& out);
esp_err_t stop_setup_ap();
bool setup_ap_active();

// True after the STA receives an IPv4 address.
bool is_connected();

} // namespace vibe_wifi
