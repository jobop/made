#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "esp_err.h"

namespace vibe_wifi {

// Call once after nvs_flash_init(). It starts a STA without waiting for a
// connection and reuses any ESP-IDF Wi-Fi configuration already in NVS.
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
