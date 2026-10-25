// sim/stubs/components_stubs.cpp — 非 UI 组件的 host 替身。
// 覆盖 vibe_wifi / vibe_pairing / vibe_phone_setup / vibe_usb /
// vibe_voice / board_voice_audio / new_message_chime。
// made_sprite_cache 已由真实现直接参与 host 构建，不再打桩。
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

#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <cstdio>

/* ---------------- board_voice_audio 宿主麦克风 ----------------
 * 设 MADE_SIM_MIC_WAV 指向 16k/单声道/s16 WAV 文件，bsp_extra_i2s_read 就按
 * 实时节奏把文件内容当成麦克风流（循环播放，封成 ES7210 的 4 槽 TDM 帧，
 * 槽 0/2 同音）。不设则维持「无麦克风」。sim 与 Android 共用这一份实现。 */
namespace {
struct HostMic {
    bool tried = false;
    bool ok = false;
    std::vector<int16_t> samples;
    size_t delivered = 0;
    std::chrono::steady_clock::time_point start{};

    void ensure()
    {
        if (tried) return;
        tried = true;
        const char *path = getenv("MADE_SIM_MIC_WAV");
#ifdef __ANDROID__
        // Android 上没有 adb 可设的进程环境，约定固定路径：文件存在即麦克风源。
        const char *android_default = "/data/user/0/org.libsdl.app/files/mic.wav";
        if (!path || !path[0]) path = android_default;
#endif
        if (!path || !path[0]) return;
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "sim-mic: cannot open %s\n", path); return; }
        std::vector<uint8_t> bytes;
        uint8_t buf[8192]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        fclose(f);
        // 最小 RIFF 解析：找 fmt 与 data 块，只收 PCM 16-bit。
        if (bytes.size() < 44 || memcmp(bytes.data(), "RIFF", 4) != 0 ||
            memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
            fprintf(stderr, "sim-mic: %s is not a WAV file\n", path); return;
        }
        uint32_t rate = 0; bool pcm16 = false; int channels = 1; size_t data_off = 0, data_len = 0;
        size_t p = 12;
        while (p + 8 <= bytes.size()) {
            const char *id = reinterpret_cast<const char *>(bytes.data() + p);
            const uint32_t size = (uint32_t)bytes[p+4] | (uint32_t)bytes[p+5] << 8 |
                                  (uint32_t)bytes[p+6] << 16 | (uint32_t)bytes[p+7] << 24;
            if (!memcmp(id, "fmt ", 4) && size >= 16) {
                const uint16_t fmt = (uint16_t)bytes[p+8] | (uint16_t)bytes[p+9] << 8;
                rate = (uint32_t)bytes[p+12] | (uint32_t)bytes[p+13] << 8 |
                       (uint32_t)bytes[p+14] << 16 | (uint32_t)bytes[p+15] << 24;
                pcm16 = fmt == 1;
                channels = (int)bytes[p+10] | (int)bytes[p+11] << 8;
                if (channels < 1) channels = 1;
            } else if (!memcmp(id, "data", 4)) {
                data_off = p + 8;
                data_len = size && p + 8 + size <= bytes.size() ? size : bytes.size() - p - 8;
            }
            p += 8 + size + (size & 1);
        }
        if (!pcm16 || rate != 16000 || data_len < 2) {
            fprintf(stderr, "sim-mic: %s must be PCM16/16kHz (rate=%u pcm=%d)\n", path, rate, (int)pcm16);
            return;
        }
        const size_t frame_bytes = 2 * (size_t)channels;
        const size_t frame_count = data_len / frame_bytes;
        samples.resize(frame_count);  // 立体声取左声道
        for (size_t i = 0; i < frame_count; ++i) {
            const uint8_t *s = bytes.data() + data_off + i * frame_bytes;
            samples[i] = (int16_t)((uint16_t)s[0] | (uint16_t)s[1] << 8);
        }
        ok = true;
        fprintf(stderr, "sim-mic: %u samples from %s\n", (unsigned)samples.size(), path);
    }
};
HostMic &host_mic() { static HostMic m; return m; }
} // namespace

extern "C" {

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
{ (void)rate; (void)bits; (void)channels; (void)slot_mask; (void)gain_mask; return ESP_OK; }
esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels)
{ (void)rate; (void)bits; (void)channels; return ESP_ERR_NOT_SUPPORTED; }
int bsp_extra_out_volume_get(void) { return 50; }
esp_err_t bsp_extra_out_volume_set(int volume) { (void)volume; return ESP_OK; }
esp_err_t bsp_extra_codec_dev_stop(void) { return ESP_OK; }
esp_err_t bsp_extra_codec_mute_set(bool muted) { (void)muted; return ESP_OK; }
esp_err_t bsp_extra_i2s_read(void *buffer, size_t length, size_t *bytes_read, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!buffer || !bytes_read) return ESP_ERR_INVALID_ARG;
    HostMic &m = host_mic();
    m.ensure();
    if (!m.ok || m.samples.empty()) { *bytes_read = 0; return ESP_ERR_INVALID_STATE; }
    const size_t frames = length / (4 * sizeof(int16_t));
    if (frames == 0) { *bytes_read = 0; return ESP_ERR_INVALID_ARG; }
    // 实时节奏：每毫秒 16 帧。首块启动计时，领先就睡到点。
    if (m.delivered == 0) m.start = std::chrono::steady_clock::now();
    const auto due = m.start + std::chrono::milliseconds((m.delivered + frames) / 16);
    std::this_thread::sleep_until(due);
    auto *out = static_cast<int16_t *>(buffer);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t s = m.samples[m.delivered % m.samples.size()];
        out[i * 4 + 0] = s;  // 槽 0
        out[i * 4 + 1] = 0;
        out[i * 4 + 2] = s;  // 槽 2
        out[i * 4 + 3] = 0;
        ++m.delivered;
    }
    *bytes_read = frames * 4 * sizeof(int16_t);
    return ESP_OK;
}
esp_err_t bsp_extra_i2s_write(void *buffer, size_t length, size_t *bytes_written, uint32_t timeout_ms)
{ (void)buffer; (void)length; (void)timeout_ms; *bytes_written = 0; return ESP_ERR_INVALID_STATE; }

} // extern "C"

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
uint32_t voluntary_disconnects() { return 0; }
std::string disconnect_history() { return {}; }

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
void begin_high_throughput() {}
void end_high_throughput() {}
} // namespace vibe_wifi

/* vibe_pairing 不再桩化：真实实现 made/components/vibe_pairing/vibe_pairing.cpp
 * 直接参与宿主构建（配对、NVS 持久化、HMAC 握手全部真跑），
 * 其依赖（esp_mac/esp_random/sntp/mbedtls_md_hmac）由 esp_stubs.cpp 提供。 */

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

// vibe_voice 不在这里桩了：真实现 vibe_voice_capture.cpp 已直接进
// sim/Makefile 与 Android CMakeLists（录音走 HostMic WAV 桩 + marker BOOT 键）。
/* ---------------- vibe_motion（host 无 IMU，摇晃感知不启动） ---------------- */
#include "vibe_motion.hpp"
namespace vibe_motion {
void start(ShakeCallback callback, void *ctx) { (void)callback; (void)ctx; }
} // namespace vibe_motion

/* ---------------- new_message_chime（host 无音频任务，提示音为空操作） ---------------- */
void made_chime_new_message() {}
void made_chime_preview() {}
bool made_chime_play_wav(const uint8_t *data, size_t size) { (void)data; (void)size; return false; }
