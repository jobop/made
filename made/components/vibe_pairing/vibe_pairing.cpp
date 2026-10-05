#include "vibe_i18n.hpp"
#include "vibe_pairing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "vibe_wifi.hpp"
#include "vibe_usb.hpp"

namespace vibe_pairing {
namespace {

constexpr char kTag[] = "vibe_pairing";
constexpr char kNvsNamespace[] = "vibe_pair";
constexpr char kReceiverUrl[] = "http://192.168.4.1:8788";
constexpr char kUsbUrl[] = "http://usb.vibe.local:8788";
constexpr uint16_t kDiscoveryPort = 8789;
constexpr char kDiscoveryRequest[] = "VIBE_DISCOVER_V1";
constexpr char kDiscoveryRequestV3[] = "VIBE_DISCOVER_V3";
constexpr size_t kBridgeLimit = 12;
constexpr char kPairedDiscoveryRequest[] = "VIBE_DISCOVER_PAIRED_V2";
constexpr char kPairedDiscoveryReply[] = "VIBE_BRIDGE_PAIRED_V2";
constexpr char kManualVerification[] = "VIBE_BRIDGE_MANUAL_V2";
constexpr size_t kResponseLimit = 2048;
constexpr int64_t kRetryMs = 5000;
constexpr int64_t kPairLifetimeMs = 120000;
constexpr int64_t kClockWaitMs = 15000;

std::mutex state_mutex;
Snapshot state;
std::once_flag load_once;
std::string device_id;
std::string nonce;
int64_t last_attempt_ms = 0;
int64_t pair_start_ms = 0;
std::atomic<bool> retry_requested{false};
std::atomic<bool> revalidation_pending{false};
std::atomic<uint32_t> authorized_epoch{UINT32_MAX};
std::atomic<uint32_t> foreground_epoch{0};
std::mutex authorization_mutex;
std::atomic<bool> foreground_active{false};
std::atomic<uint32_t> authorized_usb_epoch{UINT32_MAX};
uint32_t tick_epoch = 0; // Only read or written by the Vibe worker.
uint32_t tick_usb_epoch = 0;
uint32_t observed_usb_epoch = UINT32_MAX;
AccessMode tick_mode = AccessMode::Automatic;
std::mutex access_mutex;
struct AccessCommand {
    bool pending = false;
    AccessMode mode = AccessMode::Automatic;
    std::string url;
    bool chooser = false;
    Bridge selection;
    uint32_t epoch = 0;
    bool ephemeral = false;
};
AccessCommand access_command;
// Only the worker reads/writes the chosen target; UI commands carry an epoch
// so a selection from a closed app or a different transport can never run.
Bridge selected_bridge;
bool force_chooser = false;
uint32_t choice_epoch = UINT32_MAX;
uint32_t list_epoch = UINT32_MAX; // Protected by state_mutex.
std::string saved_token;
std::string saved_bridge;
struct SavedPair { std::string token; std::string bridge; };
std::vector<SavedPair> saved_candidates;
constexpr std::array<AccessMode, 3> kCredentialModes = {
    AccessMode::Automatic, AccessMode::Receiver, AccessMode::UsbDirect
};
bool sntp_started = false;
int64_t sntp_start_ms = 0;
std::atomic<bool> clock_synced{false};

int64_t now_ms() { return esp_timer_get_time() / 1000; }
bool current_tick_valid()
{
    return foreground_active.load() && tick_epoch == foreground_epoch.load() &&
        !revalidation_pending.load() &&
        (tick_mode != AccessMode::UsbDirect ||
         (vibe_usb::active() && vibe_usb::connected() &&
          tick_usb_epoch == vibe_usb::connection_epoch()));
}

void invalidate_foreground()
{
    std::lock_guard<std::mutex> lock(authorization_mutex);
    foreground_epoch.fetch_add(1);
    authorized_epoch.store(UINT32_MAX);
    authorized_usb_epoch.store(UINT32_MAX);
    revalidation_pending.store(true);
    vibe_usb::revoke_authorization();
}

std::string initial_url(AccessMode mode, const std::string &manual_url)
{
    if (mode == AccessMode::Manual) return manual_url;
    if (mode == AccessMode::Receiver) return kReceiverUrl;
    if (mode == AccessMode::UsbDirect) return kUsbUrl;
    return {};
}

std::string verification_message(AccessMode mode)
{
    if (mode == AccessMode::Manual) return "正在验证指定电脑";
    if (mode == AccessMode::Receiver) return "正在验证 USB 接收端";
    if (mode == AccessMode::UsbDirect) return "正在验证 USB 直连电脑";
    return "正在验证局域网电脑";
}

void authorize_current_tick(const std::string &token)
{
    std::lock_guard<std::mutex> lock(authorization_mutex);
    if (!current_tick_valid()) return;
    if (tick_mode == AccessMode::UsbDirect) {
        if (!vibe_usb::authorize_connection(tick_usb_epoch, "Bearer " + token)) return;
        authorized_usb_epoch.store(tick_usb_epoch);
    }
    authorized_epoch.store(tick_epoch);
}

bool secure_url(const std::string &url) { return url.rfind("https://", 0) == 0; }

std::string authority_for_url(const std::string &url)
{
    const bool https = secure_url(url);
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    std::string authority = url.substr(scheme + 3);
    std::transform(authority.begin(), authority.end(), authority.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const char *default_port = https ? ":443" : ":80";
    const size_t suffix_length = std::strlen(default_port);
    if (authority.size() > suffix_length &&
        authority.compare(authority.size() - suffix_length, suffix_length, default_port) == 0)
        authority.resize(authority.size() - suffix_length);
    return authority;
}

bool local_http_host(const std::string &host)
{
    in_addr ipv4 = {};
    if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1) {
        const uint32_t address = ntohl(ipv4.s_addr);
        return (address >> 24) == 10 || (address >> 20) == 0xac1 ||
               (address >> 16) == 0xc0a8 || (address >> 24) == 127 ||
               (address >> 16) == 0xa9fe;
    }
    return host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0;
}

bool valid_host(const std::string &host)
{
    if (host.empty() || host.size() > 100 || host.front() == '.' || host.back() == '.') return false;
    size_t label_length = 0;
    for (size_t i = 0; i < host.size(); ++i) {
        const unsigned char ch = host[i];
        if (ch == '.') {
            if (!label_length || label_length > 63 || host[i - 1] == '-') return false;
            label_length = 0;
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-') {
            if (!label_length && ch == '-') return false;
            ++label_length;
        } else return false;
    }
    return label_length > 0 && label_length <= 63 && host.back() != '-';
}

void begin_sntp_if_needed()
{
    if (sntp_started) return;
    if (sntp_start_ms == 0) sntp_start_ms = now_ms();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = [](struct timeval *value) {
        if (value && value->tv_sec >= 1704067200) clock_synced.store(true);
    };
    const esp_err_t result = esp_netif_sntp_init(&config);
    if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
        sntp_started = true;
    }
    else ESP_LOGW(kTag, "Cannot start HTTPS clock sync: %s", esp_err_to_name(result));
}

void publish(Phase phase, std::string message, std::string code = {}, bool retry_required = false)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    if (state.phase == phase && state.message == message && state.code == code &&
        state.retry_required == retry_required) return;
    state.phase = phase;
    state.message = std::move(message);
    state.code = std::move(code);
    state.retry_required = retry_required;
    ++state.revision;
}

bool valid_bridge_id(const std::string &id)
{
    if (id.empty() || id.size() > 64) return false;
    for (const unsigned char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return false;
    }
    return true;
}

std::string read_str(nvs_handle_t handle, const char *key, size_t capacity)
{
    std::string value(capacity, '\0');
    size_t size = capacity;
    if (nvs_get_str(handle, key, value.data(), &size) != ESP_OK || size == 0) return {};
    value.resize(size - 1);
    return value;
}

const char *token_key(AccessMode mode)
{
    if (mode == AccessMode::Receiver) return "rx_token";
    if (mode == AccessMode::UsbDirect) return "usb_token";
    return "token";
}

const char *bridge_key(AccessMode mode)
{
    if (mode == AccessMode::Receiver) return "rx_bridge";
    if (mode == AccessMode::UsbDirect) return "usb_bridge";
    return "bridge";
}

void load_saved_pair(AccessMode mode)
{
    saved_token.clear();
    saved_bridge.clear();
    saved_candidates.clear();
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    auto read_candidate = [&](AccessMode candidate_mode) {
        SavedPair pair{read_str(handle, token_key(candidate_mode), 129),
                       read_str(handle, bridge_key(candidate_mode), 65)};
        if (pair.token.size() != 64 || !valid_bridge_id(pair.bridge)) return;
        for (const auto &candidate : saved_candidates)
            if (candidate.token == pair.token && candidate.bridge == pair.bridge) return;
        saved_candidates.push_back(std::move(pair));
    };
    read_candidate(mode);
    // An empty transport slot may reuse a pairing only after the connected PC
    // proves possession of its token. None of these candidates are bearer data.
    if (saved_candidates.empty())
        for (AccessMode other : kCredentialModes) read_candidate(other);
    nvs_close(handle);
    if (!saved_candidates.empty()) {
        saved_token = saved_candidates.front().token;
        saved_bridge = saved_candidates.front().bridge;
    }
}

void load()
{
    std::call_once(load_once, [] {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char id[13] = {};
        std::snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        device_id = id;
    }

    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    const std::string manual_url = read_str(handle, "manual", 160);
    uint8_t mode = 0;
    (void)nvs_get_u8(handle, "mode", &mode);
    nvs_close(handle);
    std::lock_guard<std::mutex> lock(state_mutex);
    state.access_mode = mode == 1 ? AccessMode::Manual :
        mode == 2 ? AccessMode::Receiver :
        mode == 3 ? AccessMode::UsbDirect : AccessMode::Automatic;
    state.manual_url = manual_url;
    load_saved_pair(state.access_mode);
    // Saved credentials are only candidates. Each foreground entry proves the
    // bridge identity afresh before any bearer token is exposed to the URL.
    state.url = initial_url(state.access_mode, manual_url);
    state.phase = Phase::Discovering;
    state.message = verification_message(state.access_mode);
    ++state.revision;
    });
}

bool save_access(AccessMode mode, const std::string &url)
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = nvs_set_u8(handle, "mode", mode == AccessMode::Manual ? 1 :
                                mode == AccessMode::Receiver ? 2 :
                                mode == AccessMode::UsbDirect ? 3 : 0);
    if (error == ESP_OK && mode == AccessMode::Manual)
        error = nvs_set_str(handle, "manual", url.c_str());
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

void apply_access_command()
{
    AccessCommand command;
    {
        std::lock_guard<std::mutex> lock(access_mutex);
        if (!access_command.pending) return;
        command = std::move(access_command);
        access_command = {};
    }
    if (command.ephemeral && (!foreground_active.load() ||
                              command.epoch != foreground_epoch.load())) return;
    selected_bridge = std::move(command.selection);
    force_chooser = command.chooser;
    choice_epoch = command.epoch;
    nonce.clear();
    last_attempt_ms = 0;
    authorized_epoch.store(UINT32_MAX);
    authorized_usb_epoch.store(UINT32_MAX);
    load_saved_pair(command.mode);
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        state.access_mode = command.mode;
        if (command.mode == AccessMode::Manual) state.manual_url = command.url;
        state.url = selected_bridge.url.empty() ? initial_url(command.mode, command.url) : selected_bridge.url;
        state.token.clear();
        state.bridge_id = selected_bridge.id;
        state.code.clear();
        state.retry_required = false;
        state.bridges.clear();
        state.scanning = command.chooser;
        list_epoch = UINT32_MAX;
        state.phase = command.chooser ? Phase::ChoosingBridge : Phase::Discovering;
        state.message = command.chooser ? "正在扫描电脑" : verification_message(command.mode);
        ++state.revision;
    }
}

bool save(const std::string &url, const std::string &token, const std::string &bridge)
{
    const AccessMode mode = snapshot().access_mode;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = ESP_OK;
    if (mode == AccessMode::Automatic || mode == AccessMode::Manual)
        error = nvs_set_str(handle, "url", url.c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, token_key(mode), token.c_str());
    if (error == ESP_OK) error = nvs_set_str(handle, bridge_key(mode), bridge.c_str());
    // The desktop has one token per physical display. If an approved pairing
    // rotates that token, keep all existing transport slots for that PC in sync.
    // Empty slots remain candidates until that transport verifies the PC itself.
    for (AccessMode other : kCredentialModes) {
        if (std::strcmp(token_key(other), token_key(mode)) == 0) continue;
        if (error == ESP_OK && read_str(handle, bridge_key(other), 65) == bridge)
            error = nvs_set_str(handle, token_key(other), token.c_str());
    }
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

struct Candidate { std::string url; std::string bridge_id; };

int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool decode_hex(const std::string &hex, uint8_t *bytes, size_t size)
{
    if (hex.size() != size * 2) return false;
    for (size_t i = 0; i < size; ++i) {
        const int high = hex_nibble(hex[i * 2]);
        const int low = hex_nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool verify_paired_reply(const std::string &token, const std::string &challenge,
                         unsigned int port, const char *bridge, const char *address,
                         const char *received_mac)
{
    std::array<uint8_t, 32> key = {};
    std::array<uint8_t, 32> received = {};
    if (!decode_hex(token, key.data(), key.size()) ||
        !decode_hex(received_mac, received.data(), received.size())) return false;
    char signed_text[220] = {};
    const int length = std::snprintf(signed_text, sizeof(signed_text),
        "%s\n%s\n%s\n%u\n%s\n%s", kPairedDiscoveryReply, device_id.c_str(),
        challenge.c_str(), port, bridge, address);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(signed_text)) return false;
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!sha256) return false;
    std::array<uint8_t, 32> expected = {};
    if (mbedtls_md_hmac(sha256, key.data(), key.size(),
                        reinterpret_cast<const unsigned char *>(signed_text),
                        static_cast<size_t>(length), expected.data()) != 0) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < expected.size(); ++i) difference |= expected[i] ^ received[i];
    return difference == 0;
}

int discovery_socket()
{
    if (!current_tick_valid()) return -1;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) return -1;
    const int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
    return fd;
}

sockaddr_in discovery_destination()
{
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kDiscoveryPort);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    return destination;
}

// Keep the complete scan bounded even if many replies arrive or somebody
// floods the port. Short read slices also let exit/mode changes cancel quickly.
int discovery_receive(int fd, char *reply, size_t capacity, sockaddr_in &sender,
                      int64_t deadline)
{
    if (!current_tick_valid()) return 0;
    const int64_t remaining = deadline - now_ms();
    if (remaining <= 0) return 0;
    const int64_t wait_ms = std::min<int64_t>(remaining, 200);
    timeval timeout = {};
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(wait_ms * 1000);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    socklen_t sender_size = sizeof(sender);
    const int length = recvfrom(fd, reply, capacity - 1, 0,
                                reinterpret_cast<sockaddr *>(&sender), &sender_size);
    if (!current_tick_valid() || length <= 0) return 0;
    reply[length] = '\0';
    if (static_cast<size_t>(length) == capacity - 1 ||
        std::memchr(reply, '\0', static_cast<size_t>(length))) return 0;
    return length;
}

Candidate discover(const std::string &wanted_bridge, const std::string &token)
{
    if (!valid_bridge_id(wanted_bridge) || device_id.empty() || token.size() != 64) return {};
    const int fd = discovery_socket();
    if (fd < 0) return {};
    sockaddr_in destination = discovery_destination();
    Candidate found;
    for (int attempt = 0; attempt < 2 && current_tick_valid(); ++attempt) {
        std::array<uint8_t, 16> random_bytes = {};
        esp_fill_random(random_bytes.data(), random_bytes.size());
        char nonce_buffer[33] = {};
        for (size_t i = 0; i < random_bytes.size(); ++i)
            std::snprintf(nonce_buffer + i * 2, sizeof(nonce_buffer) - i * 2,
                          "%02x", random_bytes[i]);
        const std::string challenge = nonce_buffer;
        const std::string request = std::string(kPairedDiscoveryRequest) + " " + device_id + " " + challenge;
        if (!current_tick_valid()) break;
        (void)sendto(fd, request.data(), request.size(), 0,
                     reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
        const int64_t deadline = now_ms() + 850;
        while (now_ms() < deadline && current_tick_valid()) {
            char reply[256] = {};
            sockaddr_in sender = {};
            if (!discovery_receive(fd, reply, sizeof(reply), sender, deadline)) continue;
            char address[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address))) continue;
            unsigned int port = 0;
            char bridge[65] = {}, signed_address[INET_ADDRSTRLEN] = {}, mac[65] = {};
            char trailing = '\0';
            if (std::sscanf(reply, "VIBE_BRIDGE_PAIRED_V2 %u %64s %15s %64s %c",
                            &port, bridge, signed_address, mac, &trailing) != 4 ||
                port == 0 || port > 65535 || wanted_bridge != bridge ||
                std::strcmp(signed_address, address) != 0 ||
                !verify_paired_reply(token, challenge, port, bridge, address, mac)) continue;
            found = {std::string("http://") + address + ":" + std::to_string(port), bridge};
            break;
        }
        if (!found.url.empty()) break;
    }
    close(fd);
    return current_tick_valid() ? found : Candidate{};
}

bool valid_display_name(const std::string &name)
{
    if (name.empty() || name.size() > 96) return false;
    for (size_t i = 0; i < name.size();) {
        const uint8_t first = static_cast<uint8_t>(name[i++]);
        uint32_t value = first;
        size_t following = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { value = first & 0x1f; following = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 0x0f; following = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; following = 3; minimum = 0x10000; }
        else if (first >= 0x80) return false;
        if (i + following > name.size()) return false;
        while (following--) {
            const uint8_t next = static_cast<uint8_t>(name[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            value < 0x20 || (value >= 0x7f && value <= 0x9f) ||
            (value >= 0x202a && value <= 0x202e) || (value >= 0x2066 && value <= 0x2069)) return false;
    }
    return true;
}

struct DiscoveredBridge { Bridge bridge; bool modern = false; };

bool parse_discovered_bridge(const char *reply, const char *address, DiscoveredBridge &result)
{
    unsigned int port = 0;
    if (*reply == '{') {
        // cJSON exposes NUL-terminated strings, so reject an escaped NUL before
        // parsing rather than silently accepting a truncated name/identity.
        if (std::strstr(reply, "\\u0000")) return false;
        const char *end = nullptr;
        cJSON *root = cJSON_ParseWithOpts(reply, &end, true);
        if (!root) return false;
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        const cJSON *port_value = cJSON_GetObjectItemCaseSensitive(root, "port");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "bridgeId");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *open = cJSON_GetObjectItemCaseSensitive(root, "pairingOpen");
        const bool valid = cJSON_IsObject(root) && cJSON_IsString(type) && type->valuestring &&
            std::strcmp(type->valuestring, "VIBE_BRIDGE_V3") == 0 &&
            cJSON_IsNumber(version) && version->valuedouble == 3 &&
            cJSON_IsNumber(port_value) && port_value->valuedouble >= 1 &&
            port_value->valuedouble <= 65535 &&
            port_value->valuedouble == static_cast<unsigned int>(port_value->valuedouble) &&
            cJSON_IsString(id) && id->valuestring && valid_bridge_id(id->valuestring) &&
            cJSON_IsString(name) && name->valuestring && valid_display_name(name->valuestring) &&
            cJSON_IsBool(open);
        if (valid) {
            port = static_cast<unsigned int>(port_value->valuedouble);
            result.bridge = {id->valuestring, name->valuestring, {}, cJSON_IsTrue(open) != 0};
            result.modern = true;
        }
        cJSON_Delete(root);
        if (!valid) return false;
    } else {
        char id[65] = {}, trailing = '\0';
        if (std::sscanf(reply, "VIBE_BRIDGE_V1 %u %64s %c", &port, id, &trailing) != 2 ||
            port == 0 || port > 65535 || !valid_bridge_id(id)) return false;
        result.bridge = {id, std::string("电脑 ") + address, {}, true, true};
        result.modern = false;
    }
    // Discovery metadata cannot redirect the display to an advertised host.
    // The chosen endpoint is always the packet sender and its validated port.
    result.bridge.url = std::string("http://") + address + ":" + std::to_string(port);
    return true;
}

void collect_bridge(std::vector<DiscoveredBridge> &found, DiscoveredBridge candidate)
{
    for (auto &existing : found) {
        if (existing.bridge.id != candidate.bridge.id && existing.bridge.url != candidate.bridge.url) continue;
        // Deduplicate one computer's interfaces and V1/V3 replies. Prefer the
        // named V3 record; otherwise choose one endpoint deterministically.
        if ((candidate.modern && !existing.modern) ||
            (candidate.modern == existing.modern && candidate.bridge.url < existing.bridge.url))
            existing = std::move(candidate);
        return;
    }
    if (found.size() < kBridgeLimit) found.push_back(std::move(candidate));
}

void scan_and_publish_bridges()
{
    if (!current_tick_valid()) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!current_tick_valid()) return;
        state.phase = Phase::ChoosingBridge;
        state.scanning = true;
        state.bridges.clear();
        state.code.clear();
        state.message = "正在扫描电脑";
        list_epoch = UINT32_MAX;
        ++state.revision;
    }
    std::vector<DiscoveredBridge> found;
    const int fd = discovery_socket();
    if (fd >= 0) {
        sockaddr_in destination = discovery_destination();
        for (int attempt = 0; attempt < 2 && current_tick_valid(); ++attempt) {
            for (const char *request : {kDiscoveryRequestV3, kDiscoveryRequest}) {
                if (!current_tick_valid()) break;
                (void)sendto(fd, request, std::strlen(request), 0,
                             reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
            }
            const int64_t deadline = now_ms() + 850;
            while (now_ms() < deadline && current_tick_valid()) {
                char reply[768] = {};
                sockaddr_in sender = {};
                if (!discovery_receive(fd, reply, sizeof(reply), sender, deadline)) continue;
                char address[INET_ADDRSTRLEN] = {};
                if (!inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address))) continue;
                const uint32_t source = ntohl(sender.sin_addr.s_addr);
                if (source == 0 || source == 0xffffffff || (source >> 28) >= 0xe) continue;
                DiscoveredBridge candidate;
                if (parse_discovered_bridge(reply, address, candidate)) collect_bridge(found, std::move(candidate));
            }
        }
        close(fd);
    }
    if (!current_tick_valid()) return;
    std::sort(found.begin(), found.end(), [](const DiscoveredBridge &a, const DiscoveredBridge &b) {
        if (a.bridge.name != b.bridge.name) return a.bridge.name < b.bridge.name;
        if (a.bridge.id != b.bridge.id) return a.bridge.id < b.bridge.id;
        return a.bridge.url < b.bridge.url;
    });
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!current_tick_valid()) return;
    state.bridges.clear();
    for (auto &entry : found) state.bridges.push_back(std::move(entry.bridge));
    state.phase = Phase::ChoosingBridge;
    state.scanning = false;
    state.message = state.bridges.empty() ? "未发现电脑，请检查网络后刷新" : "请选择要连接的电脑";
    list_epoch = tick_epoch;
    ++state.revision;
}

esp_err_t on_http(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || event->data_len <= 0) return ESP_OK;
    auto *body = static_cast<std::string *>(event->user_data);
    if (body->size() + static_cast<size_t>(event->data_len) > kResponseLimit) return ESP_FAIL;
    body->append(static_cast<const char *>(event->data), event->data_len);
    return ESP_OK;
}

bool http_json(const std::string &url, const char *post_body, int &status, std::string &response)
{
    const std::string usb_prefix = std::string(kUsbUrl) + "/";
    if (url.rfind(usb_prefix, 0) == 0) {
        if (tick_mode != AccessMode::UsbDirect || !current_tick_valid()) return false;
        return vibe_usb::request(url.substr(std::strlen(kUsbUrl)), post_body != nullptr,
                                 {}, post_body ? "application/json" : "",
                                 reinterpret_cast<const uint8_t *>(post_body),
                                 post_body ? std::strlen(post_body) : 0,
                                 response, status, kResponseLimit, 4500) == ESP_OK && current_tick_valid();
    }
    // A pending mode switch must never route the USB authority through DNS.
    if (tick_mode == AccessMode::UsbDirect || !current_tick_valid()) return false;
    if (secure_url(url) && !tls_time_ready()) return false;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.disable_auto_redirect = true;
    if (secure_url(url)) config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 4500;
    config.event_handler = on_http;
    config.user_data = &response;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "ngrok-skip-browser-warning", "1");
    if (post_body) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_body, std::strlen(post_body));
    }
    const esp_err_t error = esp_http_client_perform(client);
    status = error == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    return error == ESP_OK;
}

std::string string_field(const cJSON *root, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool verify_saved_candidate_at(const std::string &url, const SavedPair &candidate)
{
    if (candidate.token.size() != 64 || !valid_bridge_id(candidate.bridge) || device_id.size() != 12)
        return false;
    std::array<uint8_t, 32> key = {};
    if (!decode_hex(candidate.token, key.data(), key.size())) return false;
    std::array<uint8_t, 16> random_bytes = {};
    esp_fill_random(random_bytes.data(), random_bytes.size());
    char challenge[33] = {};
    for (size_t i = 0; i < random_bytes.size(); ++i)
        std::snprintf(challenge + i * 2, sizeof(challenge) - i * 2, "%02x", random_bytes[i]);
    int status = 0;
    std::string response;
    if (!http_json(url + "/pair/verify?deviceId=" + device_id + "&nonce=" + challenge,
                   nullptr, status, response) || status != 200) return false;
    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) return false;
    const std::string bridge = string_field(root, "bridgeId");
    const std::string mac = string_field(root, "mac");
    const std::string authority = string_field(root, "authority");
    cJSON_Delete(root);
    if (bridge != candidate.bridge || authority != authority_for_url(url)) return false;
    std::array<uint8_t, 32> received = {};
    if (!decode_hex(mac, received.data(), received.size())) return false;
    const std::string signed_text = std::string(kManualVerification) + "\n" +
        device_id + "\n" + challenge + "\n" + bridge + "\n" + authority;
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!sha256) return false;
    std::array<uint8_t, 32> expected = {};
    if (mbedtls_md_hmac(sha256, key.data(), key.size(),
                        reinterpret_cast<const unsigned char *>(signed_text.data()),
                        signed_text.size(), expected.data()) != 0) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < expected.size(); ++i) difference |= expected[i] ^ received[i];
    if (difference != 0 || !current_tick_valid() || !save(url, candidate.token, bridge)) return false;
    saved_token = candidate.token;
    saved_bridge = bridge;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        state.url = url;
        state.token = saved_token;
        state.bridge_id = bridge;
    }
    publish(Phase::Paired, "已验证原电脑，配对已恢复");
    authorize_current_tick(candidate.token);
    return true;
}

bool verify_saved_bridge_at(const std::string &url)
{
    for (const auto &candidate : saved_candidates) {
        if (!current_tick_valid()) return false;
        if (verify_saved_candidate_at(url, candidate)) return true;
    }
    return false;
}

void make_code_and_nonce(std::string &code)
{
    std::array<uint8_t, 16> random_bytes = {};
    esp_fill_random(random_bytes.data(), random_bytes.size());
    uint32_t pin = 0;
    // Rejection sampling keeps all six-digit values equally likely. The code
    // and the secret polling nonce are generated from separate random bytes.
    constexpr uint64_t kRange = uint64_t{UINT32_MAX} + 1;
    constexpr uint64_t kLimit = (kRange / 1000000) * 1000000;
    do {
        esp_fill_random(&pin, sizeof(pin));
    } while (pin >= kLimit);
    char code_buffer[7] = {};
    std::snprintf(code_buffer, sizeof(code_buffer), "%06lu", static_cast<unsigned long>(pin % 1000000));
    code = code_buffer;
    char nonce_buffer[33] = {};
    for (size_t i = 0; i < random_bytes.size(); ++i) {
        std::snprintf(nonce_buffer + i * 2, sizeof(nonce_buffer) - i * 2, "%02x", random_bytes[i]);
    }
    nonce = nonce_buffer;
}

void start_pair(const Candidate &candidate)
{
    if (!current_tick_valid()) return;
    if (device_id.empty()) {
        publish(Phase::Error, "无法读取设备编号");
        return;
    }
    std::string code;
    make_code_and_nonce(code);
    cJSON *request = cJSON_CreateObject();
    if (!request) return;
    cJSON_AddStringToObject(request, "deviceId", device_id.c_str());
    cJSON_AddStringToObject(request, "deviceName", "码得");
    cJSON_AddStringToObject(request, "code", code.c_str());
    cJSON_AddStringToObject(request, "nonce", nonce.c_str());
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!body) return;
    int status = 0;
    std::string response;
    const bool sent = http_json(candidate.url + "/pair/request", body, status, response);
    cJSON_free(body);
    if (!current_tick_valid()) return;
    if (!sent || status != 202) {
        // When recovering a saved pairing, an unavailable pairing window or
        // transient network failure does not prove that the old token is bad.
        // Keep trying signed discovery and preserve the working credential.
        if (snapshot().phase == Phase::Paired) {
            publish(Phase::Paired, status == 403 ? "请在电脑工作台打开配对" :
                    "电脑连接待恢复");
            return;
        }
        publish(Phase::Error, status == 403 ? "在电脑工作台点击添加设备" : "电脑配对请求失败");
        return;
    }
    if (!current_tick_valid()) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        state.url = candidate.url;
        state.bridge_id = candidate.bridge_id;
    }
    pair_start_ms = now_ms();
    publish(Phase::WaitingApproval, "已找到电脑，请核对配对码", code);
}

void poll_pair()
{
    const Snapshot current = snapshot();
    if (now_ms() - pair_start_ms > kPairLifetimeMs) {
        nonce.clear();
        publish(Phase::Error, "配对已超时，请点右上角重试", {}, true);
        return;
    }
    const std::string url = current.url + "/pair/status?deviceId=" + device_id + "&nonce=" + nonce;
    int status = 0;
    std::string response;
    const bool received = http_json(url, nullptr, status, response);
    if (!current_tick_valid()) return;
    if (!received || status != 200) {
        publish(Phase::WaitingApproval, "等待电脑确认，连接暂时中断", current.code);
        return;
    }
    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) return;
    const std::string status_name = string_field(root, "status");
    const std::string token = string_field(root, "token");
    const std::string bridge = string_field(root, "bridgeId");
    cJSON_Delete(root);
    if (status_name == "approved") {
        if (token.size() != 64 || !valid_bridge_id(bridge) ||
            (!current.bridge_id.empty() && bridge != current.bridge_id)) {
            publish(Phase::Error, "配对信息无效，请点右上角重试", {}, true);
            return;
        }
        if (!current_tick_valid()) return;
        if (!save(current.url, token, bridge)) {
            publish(Phase::Error, "无法保存配对信息，请重试", {}, true);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            state.token = token;
            state.bridge_id = bridge;
        }
        saved_token = token;
        saved_bridge = bridge;
        saved_candidates = {{token, bridge}};
        nonce.clear();
        publish(Phase::Paired, "已连接电脑");
        authorize_current_tick(token);
    } else if (status_name == "denied" || status_name == "expired") {
        nonce.clear();
        publish(Phase::Error, status_name == "denied" ? "电脑拒绝了配对，请点重试" :
                "配对已超时，请点右上角重试", {}, true);
    }
}

} // namespace

Snapshot snapshot()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    auto copy = state;
    copy.message = vibe_i18n::message(copy.message);
    for (auto &bridge : copy.bridges) {
        if (bridge.generated_name && bridge.name.rfind("电脑 ", 0) == 0)
            bridge.name = std::string(vibe_i18n::tr("电脑 ")) + bridge.name.substr(std::strlen("电脑 "));
    }
    return copy;
}

void initialize() { load(); }

void apply_pending_access()
{
    load();
    apply_access_command();
}

void begin_foreground_connection()
{
    load();
    foreground_active.store(true);
    invalidate_foreground();
    std::lock_guard<std::mutex> lock(state_mutex);
    state.phase = Phase::Discovering;
    state.url = initial_url(state.access_mode, state.manual_url);
    state.token.clear();
    state.bridge_id.clear();
    state.code.clear();
    state.retry_required = false;
    state.bridges.clear();
    state.scanning = false;
    list_epoch = UINT32_MAX;
    state.message = verification_message(state.access_mode);
    ++state.revision;
}

void suspend_foreground()
{
    foreground_active.store(false);
    invalidate_foreground();
}

void suspend_receiver()
{
    const AccessMode mode = snapshot().access_mode;
    if (mode == AccessMode::Receiver || mode == AccessMode::UsbDirect) suspend_foreground();
}

bool authorized_for_foreground()
{
    std::lock_guard<std::mutex> lock(authorization_mutex);
    if (!foreground_active.load() || authorized_epoch.load() != foreground_epoch.load() ||
        revalidation_pending.load()) return false;
    if (snapshot().access_mode == AccessMode::UsbDirect)
        return vibe_usb::active() && vibe_usb::connected() &&
               authorized_usb_epoch.load() == vibe_usb::connection_epoch();
    return true;
}

void tick()
{
    apply_pending_access();
    if (!foreground_active.load()) return;
    const AccessMode mode = snapshot().access_mode;
    if (mode == AccessMode::UsbDirect) {
        const uint32_t connection_epoch = vibe_usb::connection_epoch();
        if (observed_usb_epoch != connection_epoch) {
            observed_usb_epoch = connection_epoch;
            invalidate_foreground();
        }
    }
    if (revalidation_pending.exchange(false)) {
        nonce.clear();
        last_attempt_ms = 0;
        load_saved_pair(mode);
        if (choice_epoch != foreground_epoch.load()) {
            selected_bridge = {};
            force_chooser = false;
        }
        std::lock_guard<std::mutex> lock(state_mutex);
        state.phase = force_chooser ? Phase::ChoosingBridge : Phase::Discovering;
        state.url = selected_bridge.url.empty() ? initial_url(state.access_mode, state.manual_url) : selected_bridge.url;
        state.token.clear();
        state.bridge_id = selected_bridge.id;
        state.code.clear();
        state.retry_required = false;
        state.bridges.clear();
        state.scanning = force_chooser;
        list_epoch = UINT32_MAX;
        state.message = force_chooser ? "正在扫描电脑" : verification_message(state.access_mode);
        ++state.revision;
    }
    tick_epoch = foreground_epoch.load();
    tick_mode = mode;
    tick_usb_epoch = observed_usb_epoch;
    // A UI command may arrive after the worker applied the prior command.
    // Do not consume its invalidation and authorize the old transport.
    {
        std::lock_guard<std::mutex> lock(access_mutex);
        if (access_command.pending || revalidation_pending.load()) return;
    }
    if (retry_requested.exchange(false)) {
        nonce.clear();
        last_attempt_ms = 0;
        if (!tls_time_ready()) sntp_start_ms = now_ms();
        publish(Phase::Discovering, "正在重新寻找电脑");
    }
    const Snapshot current = snapshot();
    // Never probe or send a saved token through the wrong Wi-Fi network while
    // the UI is switching between the receiver hotspot and the old station.
    if (current.access_mode == AccessMode::UsbDirect) {
        if (!vibe_usb::active() || !vibe_usb::connected()) {
            publish(Phase::Discovering, "等待 USB 直连电脑");
            return;
        }
    } else if ((current.access_mode == AccessMode::Receiver) != vibe_wifi::receiver_active()) {
        publish(Phase::Discovering, "正在切换网络");
        return;
    }
    const bool needs_tls = secure_url(current.url) ||
        (current.access_mode == AccessMode::Manual && secure_url(current.manual_url));
    if (needs_tls) begin_sntp_if_needed();
    if (needs_tls && !tls_time_ready() &&
        current.phase != Phase::Paired) {
        if (tls_time_failed()) publish(Phase::Error,
            "时间同步失败，无法安全连接；检查网络后单击 BOOT 重试", {}, true);
        else publish(Phase::Discovering, "正在同步时间以校验 HTTPS 证书");
        return;
    }
    if (current.phase == Phase::Paired) return;
    if (current.phase == Phase::ChoosingBridge && !current.scanning) return;
    if (current.phase == Phase::WaitingApproval) {
        poll_pair();
        return;
    }
    if (current.retry_required) return;
    if (last_attempt_ms && now_ms() - last_attempt_ms < kRetryMs) return;
    last_attempt_ms = now_ms();
    if (current.access_mode == AccessMode::Manual) {
        if (current.manual_url.empty()) {
            publish(Phase::Error, "请在接入设置填写桥接地址");
            return;
        }
        publish(Phase::Discovering, "正在连接指定桥接地址");
        if (verify_saved_bridge_at(current.manual_url)) return;
        start_pair({current.manual_url, {}});
        return;
    }
    if (current.access_mode == AccessMode::Receiver) {
        publish(Phase::Discovering, "正在连接 USB 接收端");
        if (verify_saved_bridge_at(kReceiverUrl)) return;
        start_pair({kReceiverUrl, {}});
        return;
    }
    if (current.access_mode == AccessMode::UsbDirect) {
        publish(Phase::Discovering, "正在连接 USB 直连电脑");
        if (verify_saved_bridge_at(kUsbUrl)) return;
        start_pair({kUsbUrl, {}});
        return;
    }
    if (!selected_bridge.url.empty() && choice_epoch == tick_epoch) {
        publish(Phase::Discovering, "正在连接选中的电脑");
        for (const auto &pair : saved_candidates) {
            if (!current_tick_valid()) return;
            if (pair.bridge == selected_bridge.id &&
                verify_saved_candidate_at(selected_bridge.url, pair)) return;
        }
        start_pair({selected_bridge.url, selected_bridge.id});
        return;
    }
    if (!force_chooser) {
        publish(Phase::Discovering, "正在验证原来的电脑");
        for (const auto &pair : saved_candidates) {
            if (!current_tick_valid()) return;
            const Candidate known = discover(pair.bridge, pair.token);
            if (!known.url.empty() && current_tick_valid() &&
                save(known.url, pair.token, pair.bridge)) {
                saved_token = pair.token;
                saved_bridge = pair.bridge;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    state.url = known.url;
                    state.token = saved_token;
                    state.bridge_id = saved_bridge;
                }
                publish(Phase::Paired, "已验证原电脑，配对已恢复");
                authorize_current_tick(pair.token);
                return;
            }
        }
    }
    // An unsigned scan is only a list. Even one result needs an explicit tap;
    // it must never become a pairing request just because it replied first.
    scan_and_publish_bridges();
}

void reconnect()
{
    load();
    if (!current_tick_valid() || !authorized_for_foreground()) return;
    const Snapshot current = snapshot();
    if (current.access_mode != AccessMode::Automatic || vibe_wifi::receiver_active()) return;
    if (current.phase != Phase::Paired || current.bridge_id.empty()) return;
    if (last_attempt_ms && now_ms() - last_attempt_ms < kRetryMs) return;
    last_attempt_ms = now_ms();
    const Candidate found = discover(current.bridge_id, current.token);
    if (!current_tick_valid()) return;
    if (!found.url.empty()) {
        if (found.url == current.url) return;
        if (!save(found.url, current.token, current.bridge_id)) return;
        std::lock_guard<std::mutex> lock(state_mutex);
        state.url = found.url;
        ++state.revision;
        ESP_LOGI(kTag, "Computer address refreshed by bridge ID");
        return;
    }

    // Recovery failed: show choices again, rather than silently pairing with
    // an unsigned reply or choosing a different computer.
    if (current_tick_valid()) scan_bridges();
}

void forget()
{
    load();
    invalidate_foreground();
    const AccessMode mode = snapshot().access_mode;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        std::string forgotten_bridge = read_str(handle, bridge_key(mode), 65);
        if (forgotten_bridge.empty()) forgotten_bridge = saved_bridge;
        for (AccessMode other : kCredentialModes) {
            if (!forgotten_bridge.empty() && read_str(handle, bridge_key(other), 65) == forgotten_bridge) {
                (void)nvs_erase_key(handle, token_key(other));
                (void)nvs_erase_key(handle, bridge_key(other));
                if (other == AccessMode::Automatic) (void)nvs_erase_key(handle, "url");
            }
        }
        if (mode == AccessMode::Automatic || mode == AccessMode::Manual)
            (void)nvs_erase_key(handle, "url");
        (void)nvs_erase_key(handle, token_key(mode));
        (void)nvs_erase_key(handle, bridge_key(mode));
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
    nonce.clear();
    saved_token.clear();
    saved_bridge.clear();
    saved_candidates.clear();
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        state.url.clear();
        state.token.clear();
        state.bridge_id.clear();
    }
    publish(Phase::Error, "电脑已解除配对，准备重新连接");
    last_attempt_ms = 0;
}

void retry()
{
    const Snapshot current = snapshot();
    if (!current.retry_required) return;
    // UI callbacks only signal the worker; the worker owns nonce and timers.
    retry_requested.store(true);
}

bool tls_time_ready()
{
    // X.509 validity dates cannot be checked against the RTC's 1970 boot
    // value. A plausible RTC value alone is not proof of synchronization.
    return clock_synced.load() && std::time(nullptr) >= 1704067200;
}

bool tls_time_failed()
{
    return !tls_time_ready() && sntp_start_ms > 0 &&
           now_ms() - sntp_start_ms >= kClockWaitMs;
}

bool set_manual_endpoint(const std::string &host, uint16_t port, bool https,
                         std::string &error)
{
    if (!valid_host(host) || port == 0) {
        error = "请输入有效主机名、IPv4 地址和端口";
        return false;
    }
    if (!https && !local_http_host(host)) {
        error = "公网地址必须使用 HTTPS";
        return false;
    }
    const std::string url = std::string(https ? "https://" : "http://") + host +
        ":" + std::to_string(port);
    {
        std::lock_guard<std::mutex> lock(access_mutex);
        if (!save_access(AccessMode::Manual, url)) {
            error = "保存接入地址失败";
            return false;
        }
        invalidate_foreground();
        access_command = {true, AccessMode::Manual, url, false, {}, foreground_epoch.load(), false};
    }
    error.clear();
    return true;
}

bool use_automatic_discovery()
{
    std::lock_guard<std::mutex> lock(access_mutex);
    if (!save_access(AccessMode::Automatic, {})) return false;
    invalidate_foreground();
    access_command = {true, AccessMode::Automatic, {}, true, {}, foreground_epoch.load(), false};
    return true;
}

void scan_bridges()
{
    std::lock_guard<std::mutex> lock(access_mutex);
    const Snapshot current = snapshot();
    if (!foreground_active.load() || current.access_mode != AccessMode::Automatic ||
        (access_command.pending && access_command.mode != AccessMode::Automatic)) return;
    invalidate_foreground();
    access_command = {true, AccessMode::Automatic, {}, true, {}, foreground_epoch.load(), true};
    std::lock_guard<std::mutex> state_lock(state_mutex);
    state.phase = Phase::ChoosingBridge;
    state.bridges.clear();
    state.scanning = true;
    state.url.clear();
    state.token.clear();
    state.bridge_id.clear();
    state.code.clear();
    state.retry_required = false;
    state.message = "正在扫描电脑";
    list_epoch = UINT32_MAX;
    ++state.revision;
}

bool select_bridge(const std::string &id, const std::string &url)
{
    std::lock_guard<std::mutex> lock(access_mutex);
    if (!foreground_active.load() || access_command.pending) return false;
    Bridge selected;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex);
        if (state.access_mode != AccessMode::Automatic || state.phase != Phase::ChoosingBridge ||
            state.scanning || list_epoch != foreground_epoch.load()) return false;
        const auto found = std::find_if(state.bridges.begin(), state.bridges.end(),
            [&](const Bridge &bridge) { return bridge.id == id && bridge.url == url; });
        if (found == state.bridges.end()) return false;
        selected = *found;
    }
    invalidate_foreground();
    access_command = {true, AccessMode::Automatic, {}, false, std::move(selected),
                      foreground_epoch.load(), true};
    std::lock_guard<std::mutex> state_lock(state_mutex);
    state.phase = Phase::Discovering;
    state.scanning = false;
    state.bridges.clear();
    state.token.clear();
    state.code.clear();
    state.url = url;
    state.bridge_id = id;
    state.message = "正在连接选中的电脑";
    list_epoch = UINT32_MAX;
    ++state.revision;
    return true;
}

bool use_receiver()
{
    std::lock_guard<std::mutex> lock(access_mutex);
    if (!save_access(AccessMode::Receiver, {})) return false;
    invalidate_foreground();
    access_command = {true, AccessMode::Receiver, {}, false, {}, foreground_epoch.load(), false};
    return true;
}

bool use_usb_direct()
{
    std::lock_guard<std::mutex> lock(access_mutex);
    if (!save_access(AccessMode::UsbDirect, {})) return false;
    invalidate_foreground();
    access_command = {true, AccessMode::UsbDirect, {}, false, {}, foreground_epoch.load(), false};
    return true;
}

} // namespace vibe_pairing
