#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 1, ESP_ERR_INVALID_ARG = 2;
constexpr int ESP_ERR_NO_MEM = 3, ESP_ERR_NOT_FOUND = 4, ESP_ERR_WIFI_INIT_STATE = 5, ESP_ERR_WIFI_STATE = 6;
constexpr int ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED = 7, ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED = 8;
inline const char* esp_err_to_name(int) { return "fake error"; }
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define CONFIG_VIBE_WIFI_SSID ""
#define CONFIG_VIBE_WIFI_PASSWORD ""
using esp_event_base_t = const char*;
inline constexpr char WIFI_EVENT[] = "wifi", IP_EVENT[] = "ip";
constexpr int ESP_EVENT_ANY_ID = -1, WIFI_EVENT_STA_START = 1, WIFI_EVENT_STA_DISCONNECTED = 2;
constexpr int WIFI_EVENT_SCAN_DONE = 3, WIFI_EVENT_STA_STOP = 4, IP_EVENT_STA_GOT_IP = 1, IP_EVENT_STA_LOST_IP = 2;
using Handler = void (*)(void*, esp_event_base_t, int32_t, void*);
struct wifi_config_t {
    struct { uint8_t ssid[32]{}; uint8_t password[64]{}; } sta;
    struct {
        uint8_t ssid[32]{}; uint8_t password[64]{}; uint8_t ssid_len = 0;
        uint8_t channel = 0, max_connection = 0; int authmode = 0;
        struct { bool required = false; } pmf_cfg;
    } ap;
};
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
enum wifi_mode_t { WIFI_MODE_NULL, WIFI_MODE_AP, WIFI_MODE_STA, WIFI_MODE_APSTA };
constexpr int WIFI_STORAGE_FLASH = 0, WIFI_STORAGE_RAM = 1, WIFI_IF_STA = 0, WIFI_IF_AP = 1;
constexpr int WIFI_SCAN_TYPE_ACTIVE = 0, WIFI_AUTH_WPA2_PSK = 3;
struct esp_ip4_addr_t { uint32_t addr = 0; };
struct esp_netif_ip_info_t { esp_ip4_addr_t ip, gw, netmask; };
struct esp_netif_dns_info_t { struct { int type = 0; struct { esp_ip4_addr_t ip4; } u_addr; } ip; };
constexpr int ESP_NETIF_DNS_MAIN = 0, ESP_IPADDR_TYPE_V4 = 0, ESP_NETIF_OP_SET = 0, ESP_NETIF_OP_GET = 1;
constexpr int ESP_NETIF_DOMAIN_NAME_SERVER = 6;
enum esp_netif_dhcp_status_t { ESP_NETIF_DHCP_INIT, ESP_NETIF_DHCP_STARTED, ESP_NETIF_DHCP_STOPPED };
struct esp_netif_t {
    esp_netif_ip_info_t ip;
    esp_netif_dhcp_status_t dhcp = ESP_NETIF_DHCP_INIT;
    esp_netif_dns_info_t dns;
    uint8_t offer_dns = 0;
};
#define ESP_IP4TOADDR(a,b,c,d) ((uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d))
struct wifi_scan_config_t { bool show_hidden = false; uint8_t channel = 0; int scan_type = 0; };
struct wifi_ap_record_t { uint8_t ssid[33]{}; int8_t rssi = 0; };
struct wifi_event_sta_scan_done_t { uint32_t status = 0; uint8_t number = 0; uint8_t scan_id = 0; };
struct esp_timer_create_args_t { void (*callback)(void*); void* arg; int dispatch_method; const char* name; };
using esp_timer_handle_t = void*;
constexpr int ESP_TIMER_TASK = 0;
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
namespace fake {
inline Handler wifi_handler = nullptr, ip_handler = nullptr;
inline void (*timer_callback)(void*) = nullptr;
inline bool timer_active = false, scan_blocked = false;
inline wifi_config_t config;
inline wifi_config_t ap_config;
inline wifi_mode_t mode = WIFI_MODE_STA;
inline int storage = WIFI_STORAGE_FLASH;
inline esp_netif_t station_netif, ap_netif;
inline bool ap_exists = false;
inline bool simulate_netif_events = false;
inline int ap_creates = 0, ap_destroys = 0, ap_writes = 0, persistent_writes = 0;
inline int fail_ap_config_once = 0, fail_mode_once = 0, fail_dhcp_once = 0;
inline uint32_t random = 12345678;
inline wifi_scan_config_t scan_config;
inline std::vector<wifi_ap_record_t> records;
inline int scan_error = ESP_OK, records_error = ESP_OK;
inline int scans = 0, connects = 0, disconnects = 0, writes = 0, clear_calls = 0, record_calls = 0;
inline std::map<std::string, std::string> nvs;
inline void emit(esp_event_base_t base, int32_t id, void* data = nullptr) {
    (base == WIFI_EVENT ? wifi_handler : ip_handler)(nullptr, base, id, data);
}
inline void done(int status = 0) {
    wifi_event_sta_scan_done_t event{};
    event.status = status;
    emit(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &event);
}
inline wifi_ap_record_t ap(const char* ssid, int rssi) {
    wifi_ap_record_t record{};
    std::strncpy(reinterpret_cast<char*>(record.ssid), ssid, 32);
    record.rssi = rssi;
    return record;
}
}
inline int esp_netif_init() { return ESP_OK; }
inline int esp_event_loop_create_default() { return ESP_OK; }
inline esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key) {
    if (std::strcmp(key, "WIFI_AP_DEF") == 0) return fake::ap_exists ? &fake::ap_netif : nullptr;
    return &fake::station_netif;
}
inline esp_netif_t* esp_netif_create_default_wifi_sta() { return &fake::station_netif; }
inline esp_netif_t* esp_netif_create_default_wifi_ap() {
    // Real registration may wait for an in-progress Wi-Fi handler. Deliver an
    // event synchronously here to catch station-mutex inversion in host tests.
    if (fake::simulate_netif_events) fake::emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    ++fake::ap_creates; fake::ap_exists = true; fake::ap_netif = {}; return &fake::ap_netif;
}
inline void esp_netif_destroy_default_wifi(void*) {
    if (fake::simulate_netif_events) fake::emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    ++fake::ap_destroys; fake::ap_exists = false;
}
inline int esp_netif_get_ip_info(esp_netif_t* netif, esp_netif_ip_info_t* ip) { *ip = netif->ip; return ESP_OK; }
inline int esp_netif_set_ip_info(esp_netif_t* netif, const esp_netif_ip_info_t* ip) { netif->ip = *ip; return ESP_OK; }
inline int esp_netif_dhcps_get_status(esp_netif_t* netif, esp_netif_dhcp_status_t* status) { *status = netif->dhcp; return ESP_OK; }
inline int esp_netif_get_dns_info(esp_netif_t* netif, int, esp_netif_dns_info_t* dns) { *dns = netif->dns; return ESP_OK; }
inline int esp_netif_set_dns_info(esp_netif_t* netif, int, esp_netif_dns_info_t* dns) { netif->dns = *dns; return ESP_OK; }
inline int esp_netif_dhcps_option(esp_netif_t* netif, int op, int, void* value, size_t) {
    if (op == ESP_NETIF_OP_SET) netif->offer_dns = *static_cast<uint8_t*>(value);
    else *static_cast<uint8_t*>(value) = netif->offer_dns;
    return ESP_OK;
}
inline int esp_netif_dhcps_stop(esp_netif_t* netif) { netif->dhcp = ESP_NETIF_DHCP_STOPPED; return ESP_OK; }
inline int esp_netif_dhcps_start(esp_netif_t* netif) {
    if (fake::fail_dhcp_once) { --fake::fail_dhcp_once; return ESP_FAIL; }
    netif->dhcp = fake::mode == WIFI_MODE_STA ? ESP_NETIF_DHCP_INIT : ESP_NETIF_DHCP_STARTED;
    return ESP_OK;
}
inline int esp_wifi_init(wifi_init_config_t*) { return ESP_OK; }
inline int esp_wifi_set_storage(int storage) { fake::storage = storage; return ESP_OK; }
inline int esp_wifi_get_mode(wifi_mode_t* mode) { *mode = fake::mode; return ESP_OK; }
inline int esp_wifi_set_mode(wifi_mode_t mode) {
    if (fake::fail_mode_once) { --fake::fail_mode_once; return ESP_FAIL; }
    fake::mode = mode; return ESP_OK;
}
inline int esp_wifi_get_config(int interface, wifi_config_t* config) {
    *config = interface == WIFI_IF_AP ? fake::ap_config : fake::config; return ESP_OK;
}
inline int esp_wifi_set_config(int interface, wifi_config_t* config) {
    if (interface == WIFI_IF_AP) {
        if (config->ap.ssid_len == 0 && config->ap.ssid[0] == 0) return ESP_ERR_INVALID_ARG;
        if (fake::fail_ap_config_once) { --fake::fail_ap_config_once; return ESP_FAIL; }
        ++fake::ap_writes; fake::ap_config = *config;
    } else { ++fake::writes; fake::config = *config; }
    if (fake::storage == WIFI_STORAGE_FLASH) ++fake::persistent_writes;
    return ESP_OK;
}
inline int esp_wifi_get_mac(int, uint8_t* mac) { const uint8_t bytes[6] = {1, 2, 3, 4, 0xEF, 0x19}; std::memcpy(mac, bytes, 6); return ESP_OK; }
inline uint32_t esp_random() { return fake::random++; }
inline int esp_wifi_start() { return ESP_OK; }
inline int esp_wifi_connect() { ++fake::connects; return ESP_OK; }
inline int esp_wifi_disconnect() { ++fake::disconnects; return ESP_OK; }
inline int esp_wifi_scan_start(wifi_scan_config_t* config, bool block) {
    ++fake::scans; fake::scan_blocked = block; fake::scan_config = *config; return fake::scan_error;
}
inline int esp_wifi_scan_get_ap_num(uint16_t* count) { *count = fake::records.size(); return ESP_OK; }
inline int esp_wifi_scan_get_ap_records(uint16_t* count, wifi_ap_record_t* out) {
    ++fake::record_calls;
    if (fake::records_error != ESP_OK) return fake::records_error;
    *count = std::min<size_t>(*count, fake::records.size());
    std::copy_n(fake::records.begin(), *count, out);
    fake::records.clear();
    return ESP_OK;
}
inline int esp_wifi_clear_ap_list() { ++fake::clear_calls; fake::records.clear(); return ESP_OK; }
inline int esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* timer) {
    *timer = reinterpret_cast<void*>(1); fake::timer_callback = args->callback; return ESP_OK;
}
inline int esp_timer_start_once(esp_timer_handle_t, uint64_t) { fake::timer_active = true; return ESP_OK; }
inline int esp_timer_stop(esp_timer_handle_t) { fake::timer_active = false; return ESP_OK; }
inline int esp_timer_delete(esp_timer_handle_t) { return ESP_OK; }
inline int esp_event_handler_register(esp_event_base_t base, int, Handler handler, void*) {
    (base == WIFI_EVENT ? fake::wifi_handler : fake::ip_handler) = handler; return ESP_OK;
}
inline int esp_event_handler_unregister(esp_event_base_t, int, Handler) { return ESP_OK; }
inline int nvs_open(const char*, int, nvs_handle_t* handle) { *handle = 1; return ESP_OK; }
inline int nvs_get_str(int, const char* key, char* out, size_t* size) {
    const auto item = fake::nvs.find(key);
    if (item == fake::nvs.end()) return ESP_ERR_NOT_FOUND;
    if (*size <= item->second.size()) return ESP_ERR_NO_MEM;
    std::memcpy(out, item->second.c_str(), item->second.size() + 1); return ESP_OK;
}
inline int nvs_set_str(int, const char* key, const char* value) { fake::nvs[key] = value; return ESP_OK; }
inline int nvs_commit(int) { return ESP_OK; }
inline void nvs_close(int) {}
