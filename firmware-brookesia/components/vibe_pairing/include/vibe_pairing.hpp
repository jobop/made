#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vibe_pairing {

enum class Phase { Discovering, ChoosingBridge, WaitingApproval, Paired, Error };
enum class AccessMode { Automatic, Manual, Receiver, UsbDirect };

struct Bridge {
    std::string id;
    std::string name;
    std::string url;
    bool pairing_open = false;
    bool generated_name = false;
};

struct Snapshot {
    Phase phase = Phase::Discovering;
    std::string url;
    std::string token;
    std::string bridge_id;
    std::string code;
    std::string message = "正在寻找电脑";
    AccessMode access_mode = AccessMode::Automatic;
    std::string manual_url;
    bool retry_required = false;
    std::vector<Bridge> bridges;
    bool scanning = false;
    uint32_t revision = 0;
};

// Call tick() only from the Coding Agent worker. Network modes need Wi-Fi;
// UsbDirect runs without Wi-Fi and uses the active USB transport instead.
// The first request is made only after the app has been opened. The returned
// snapshot can safely be read from the LVGL or voice callback tasks.
void initialize(); // Read saved access mode before the settings screen opens.
void apply_pending_access(); // Worker only: run before choosing Wi-Fi/USB readiness gates.
void begin_foreground_connection(); // Revalidate saved credentials on every Vibe open/resume.
void suspend_foreground(); // Revoke authorization immediately when the app exits.
void suspend_receiver(); // Compatibility helper for Receiver and UsbDirect.
bool authorized_for_foreground();
void tick();
Snapshot snapshot();
void reconnect();           // Refresh the saved computer IP by bridge ID.
void forget();              // Used when the computer rejects a saved token.
void retry();               // User requested a new code after deny/expiry.
// The URL is bound to the chosen host. A changed endpoint must prove the saved
// computer identity, or complete six-digit pairing, before receiving a bearer.
bool set_manual_endpoint(const std::string &host, uint16_t port, bool https,
                         std::string &error);
bool use_automatic_discovery(); // Explicit settings save always opens the chooser.
// UI-safe commands. Discovery and pairing run on the worker, never on LVGL.
void scan_bridges();
bool select_bridge(const std::string &id, const std::string &url);
// Connect to the paired USB receiver at its fixed SoftAP gateway. Wi-Fi
// credentials are managed separately by vibe_wifi.
bool use_receiver();
// Connect to the desktop through the display's own USB cable, without Wi-Fi.
bool use_usb_direct();
bool tls_time_ready();
bool tls_time_failed();

} // namespace vibe_pairing
