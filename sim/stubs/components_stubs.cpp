// sim/stubs/components_stubs.cpp — 非 UI 组件的 host 替身。
// 覆盖 vibe_wifi / vibe_pairing / vibe_phone_setup / vibe_usb /
// vibe_voice / board_voice_audio / new_message_chime / made_sprite_cache。
// 环境变量：
//   MADE_SIM_PAIRING=paired   模拟已配对（UI 显示任务页 + 连接错误态）
//   MADE_SIM_PAIRING=discovering（默认）模拟未配对（配对抽屉布局）
//   MADE_SIM_WIFI=1           模拟 WiFi 已连接
#include "vibe_wifi.hpp"
#include "vibe_pairing.hpp"
#include "vibe_phone_setup.hpp"
#include "vibe_usb.hpp"
#include "vibe_voice_capture.hpp"
#include "board_voice_audio.h"
#include "new_message_chime.hpp"
#include "made_sprite_cache.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {

/* ---------------- board_voice_audio ---------------- */
esp_err_t bsp_extra_audio_session_acquire(bsp_extra_audio_owner_t owner) { (void)owner; return ESP_OK; }
esp_err_t bsp_extra_audio_session_release(bsp_extra_audio_owner_t owner) { (void)owner; return ESP_OK; }
bsp_extra_audio_owner_t bsp_extra_audio_session_get_owner(void) { return BSP_EXTRA_AUDIO_OWNER_NONE; }
const char *bsp_extra_audio_owner_name(bsp_extra_audio_owner_t owner)
{
    switch (owner) {
    case BSP_EXTRA_AUDIO_OWNER_XIAOZHI: return "xiaozhi";
    case BSP_EXTRA_AUDIO_OWNER_VIBE: return "vibe";
    default: return "none";
    }
}
esp_err_t bsp_extra_codec_set_voice_fs(uint32_t rate, uint32_t bits, uint8_t channels,
                                       uint16_t slot_mask, uint16_t gain_mask)
{ (void)rate; (void)bits; (void)channels; (void)slot_mask; (void)gain_mask; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels)
{ (void)rate; (void)bits; (void)channels; return ESP_ERR_NOT_SUPPORTED; }
int bsp_extra_out_volume_get(void) { return 50; }
esp_err_t bsp_extra_out_volume_set(int volume) { (void)volume; return ESP_OK; }
esp_err_t bsp_extra_codec_dev_stop(void) { return ESP_OK; }
esp_err_t bsp_extra_codec_mute_set(bool muted) { (void)muted; return ESP_OK; }
esp_err_t bsp_extra_i2s_read(void *buffer, size_t length, size_t *bytes_read, uint32_t timeout_ms)
{ (void)buffer; (void)length; (void)timeout_ms; *bytes_read = 0; return ESP_ERR_INVALID_STATE; }
esp_err_t bsp_extra_i2s_write(void *buffer, size_t length, size_t *bytes_written, uint32_t timeout_ms)
{ (void)buffer; (void)length; (void)timeout_ms; *bytes_written = 0; return ESP_ERR_INVALID_STATE; }

} // extern "C"

/* ---------------- new_message_chime ---------------- */
void made_chime_new_message() {}
void made_chime_preview() {}
bool made_chime_play_wav(const uint8_t *data, size_t size) { (void)data; (void)size; return false; }

/* ---------------- made_sprite_cache（素材缓存：模拟器里一律未缓存） ---------------- */
namespace made_sprite_cache {
std::string baseDir(const std::string &provider) { (void)provider; return ""; }
bool ensure(const std::string &provider, const vibe_provider::SpriteInfo &sprite, const Fetch &fetch)
{ (void)provider; (void)sprite; (void)fetch; return false; }
bool loadManifest(const std::string &provider, std::string &json) { (void)provider; (void)json; return false; }
bool cachedVersion(const std::string &provider, uint32_t &version) { (void)provider; (void)version; return false; }
void purge(const std::string &provider) { (void)provider; }
} // namespace made_sprite_cache

/* ---------------- vibe_wifi ---------------- */
namespace vibe_wifi {
namespace {
bool wifi_connected() { return getenv("MADE_SIM_WIFI") != nullptr; }
}

bool station_connected() { return wifi_connected(); }
esp_err_t start() { return ESP_OK; }
esp_err_t save_credentials(const char *ssid, const char *password) { (void)ssid; (void)password; return ESP_OK; }
esp_err_t save_receiver_credentials(const char *ssid, const char *password) { (void)ssid; (void)password; return ESP_OK; }
bool receiver_ssid(char *out, size_t capacity) { (void)capacity; if (out) out[0] = 0; return false; }
esp_err_t connect_receiver() { return ESP_ERR_INVALID_STATE; }
esp_err_t restore_station() { return ESP_OK; }
bool receiver_active() { return false; }
bool station_held() { return false; }
int disconnect_reason() { return 0; }
uint32_t disconnect_count() { return 0; }

esp_err_t request_receiver_scan() { return ESP_ERR_INVALID_STATE; }
ReceiverScanSnapshot receiver_scan_snapshot() { return {}; }
void cancel_receiver_scan() {}
esp_err_t request_station_scan()
{
    if (!wifi_connected()) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
StationScanSnapshot station_scan_snapshot()
{
    StationScanSnapshot snap;
    if (getenv("MADE_SIM_WIFI")) {
        snap.state = ReceiverScanState::Done;
        snap.networks.push_back({"SimWiFi-Home", -40});
        snap.networks.push_back({"SimWiFi-Office", -60});
        snap.generation = 1;
    }
    return snap;
}
esp_err_t start_setup_ap(SetupAccessPoint &out)
{
    out.ssid = "Vibe-Setup-Sim";
    out.password = "12345678";
    out.address = "http://192.168.4.1";
    return ESP_OK;
}
esp_err_t stop_setup_ap() { return ESP_OK; }
bool setup_ap_active() { return false; }
bool is_connected() { return wifi_connected(); }
} // namespace vibe_wifi

/* ---------------- vibe_pairing ---------------- */
namespace vibe_pairing {
namespace {
bool paired_mode() { return getenv("MADE_SIM_PAIRING") && strcmp(getenv("MADE_SIM_PAIRING"), "paired") == 0; }
Snapshot &state()
{
    static Snapshot snap = [] {
        Snapshot s;
        if (paired_mode()) {
            s.phase = Phase::Paired;
            s.url = "http://127.0.0.1:8788";
            s.token = "sim-token";
            s.bridge_id = "sim-bridge";
            s.message = "";
            s.access_mode = AccessMode::Automatic;
            s.revision = 1;
        }
        return s;
    }();
    return snap;
}
}

void initialize() {}
void apply_pending_access() {}
void begin_foreground_connection() {}
void suspend_foreground() {}
void suspend_receiver() {}
bool authorized_for_foreground() { return paired_mode(); }
void tick() {}
Snapshot snapshot() { return state(); }
void reconnect() {}
void forget() {}
void retry() {}
bool set_manual_endpoint(const std::string &host, uint16_t port, bool https, std::string &error)
{ (void)host; (void)port; (void)https; (void)error; return true; }
bool use_automatic_discovery() { return true; }
void scan_bridges() {}
bool select_bridge(const std::string &id, const std::string &url) { (void)id; (void)url; return true; }
bool use_receiver() { return false; }
bool use_usb_direct() { return false; }
bool tls_time_ready() { return true; }
bool tls_time_failed() { return false; }
} // namespace vibe_pairing

/* ---------------- vibe_phone_setup ---------------- */
namespace vibe_phone_setup {
esp_err_t start(bool english, const InitialConfig &initial) { (void)english; (void)initial; return ESP_OK; }
void stop() {}
Snapshot snapshot() { return {}; }
bool take_submission(Submission &out) { (void)out; return false; }
void tick() {}
} // namespace vibe_phone_setup

/* ---------------- vibe_usb ---------------- */
namespace vibe_usb {
bool is_direct_url(const std::string &url) { return url.rfind(kBaseUrl, 0) == 0; }
bool is_receiver_url(const std::string &url) { return url.rfind(kReceiverUrl, 0) == 0; }
esp_err_t initialize() { return ESP_OK; }
void set_active(bool enabled) { (void)enabled; }
bool active() { return false; }
bool connected() { return false; }
uint32_t connection_epoch() { return 1; }
bool authorize_connection(uint32_t expected_epoch, const std::string &authorization)
{ (void)expected_epoch; (void)authorization; return false; }
void revoke_authorization() {}
void cancel_current() {}
esp_err_t request(const std::string &path, bool post, const std::string &authorization,
                  const char *content_type, const uint8_t *body, size_t length,
                  std::string &response, int &status, size_t response_limit, int timeout_ms,
                  const std::atomic<bool> *cancellation_flag)
{ (void)path; (void)post; (void)authorization; (void)content_type; (void)body; (void)length;
  (void)response; (void)response_limit; (void)timeout_ms; (void)cancellation_flag;
  status = 0; return ESP_ERR_INVALID_STATE; }
esp_err_t receiver_request(const std::string &path, bool post, const std::string &authorization,
                           const char *content_type, const uint8_t *body, size_t length,
                           std::string &response, int &status, size_t response_limit, int timeout_ms,
                           const std::atomic<bool> *cancellation_flag)
{ (void)path; (void)post; (void)authorization; (void)content_type; (void)body; (void)length;
  (void)response; (void)response_limit; (void)timeout_ms; (void)cancellation_flag;
  status = 0; return ESP_ERR_INVALID_STATE; }
} // namespace vibe_usb

/* ---------------- vibe_voice ---------------- */
namespace vibe_voice {
bool start(const std::string &bridge_url, const std::string &token, const std::string &provider,
           const std::string &project_id, const std::string &session_id, bool hold_mode)
{ (void)bridge_url; (void)token; (void)provider; (void)project_id; (void)session_id;
  (void)hold_mode; return false; }
void finish() {}
void cancel() {}
Status status() { return Status{Phase::Idle, "", "", false}; }
} // namespace vibe_voice

/* ---------------- vibe_motion（host 无 IMU，摇晃感知不启动） ---------------- */
#include "vibe_motion.hpp"
namespace vibe_motion {
void start(ShakeCallback callback, void *ctx) { (void)callback; (void)ctx; }
} // namespace vibe_motion
