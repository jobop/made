#include "vibe_wifi.hpp"

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace vibe_wifi {
namespace {
constexpr char kTag[] = "vibe_wifi";
constexpr char kNamespace[] = "vibe_wifi";
constexpr uint32_t kMaxRetryMs = 30000;
constexpr size_t kMaxReceiverNetworks = 12;
constexpr char kReceiverPrefix[] = "VibeReceiver-";

std::atomic<bool> started{false};
std::atomic<bool> have_credentials{false};
std::atomic<bool> connected{false};
std::atomic<uint32_t> retry_ms{1000};
esp_timer_handle_t retry_timer = nullptr;
std::mutex station_mutex;
std::mutex setup_mutex;
wifi_config_t previous_station = {};
bool previous_had_credentials = false;
std::atomic<bool> using_receiver{false};
// station_mutex protects scan ownership and snapshots as well as driver
// connection changes. Only the retry gate is read without that mutex.
std::atomic<bool> receiver_scan_in_flight{false};
bool receiver_scan_cancelled = false;
ReceiverScanSnapshot receiver_scan;
std::atomic<bool> station_scan_in_flight{false};
StationScanSnapshot station_scan;
std::atomic<bool> setup_active{false};
bool setup_ready = false;
SetupAccessPoint setup_access_point;
esp_netif_t* setup_netif = nullptr;
bool setup_owns_netif = false;
wifi_mode_t setup_previous_mode = WIFI_MODE_STA;
wifi_config_t setup_previous_ap = {};
bool setup_previous_ap_configured = false;
esp_netif_ip_info_t setup_previous_ip = {};
esp_netif_dns_info_t setup_previous_dns = {};
uint8_t setup_previous_offer_dns = 0;
esp_netif_dhcp_status_t setup_previous_dhcp = ESP_NETIF_DHCP_INIT;
bool setup_saved_netif = false;
bool setup_mode_changed = false;

bool validCredentials(const char* ssid, const char* password) {
    if (ssid == nullptr || password == nullptr) return false;
    const size_t ssid_len = strnlen(ssid, 33);
    const size_t password_len = strnlen(password, 64);
    return ssid_len >= 1 && ssid_len <= 32 && password_len <= 63 &&
           (password_len == 0 || password_len >= 8);
}

bool validReceiverCredentials(const char* ssid, const char* password) {
    return validCredentials(ssid, password) && std::strlen(password) >= 8;
}

void fillConfig(wifi_config_t& config, const char* ssid, const char* password) {
    config = {};
    const size_t ssid_len = std::strlen(ssid);
    const size_t password_len = std::strlen(password);
    std::memcpy(config.sta.ssid, ssid, ssid_len);
    std::memcpy(config.sta.password, password, password_len);
}

bool readSavedConfig(wifi_config_t& config) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    char ssid[33] = {};
    char password[64] = {};
    size_t ssid_size = sizeof(ssid);
    size_t password_size = sizeof(password);
    const bool found = nvs_get_str(handle, "ssid", ssid, &ssid_size) == ESP_OK &&
                       nvs_get_str(handle, "password", password, &password_size) == ESP_OK &&
                       validCredentials(ssid, password);
    nvs_close(handle);
    if (found) fillConfig(config, ssid, password);
    // Clear the stack copy of the secret after passing it to the Wi-Fi driver.
    std::memset(password, 0, sizeof(password));
    return found;
}

bool readReceiverConfig(wifi_config_t& config) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    char ssid[33] = {};
    char password[64] = {};
    size_t ssid_size = sizeof(ssid);
    size_t password_size = sizeof(password);
    const bool found = nvs_get_str(handle, "rx_ssid", ssid, &ssid_size) == ESP_OK &&
                       nvs_get_str(handle, "rx_pass", password, &password_size) == ESP_OK &&
                       validReceiverCredentials(ssid, password);
    nvs_close(handle);
    if (found) fillConfig(config, ssid, password);
    std::memset(password, 0, sizeof(password));
    return found;
}

bool hasDriverConfig(const wifi_config_t& config) {
    return strnlen(reinterpret_cast<const char*>(config.sta.ssid), sizeof(config.sta.ssid)) > 0;
}

void scheduleRetry(uint32_t delay_ms) {
    if (retry_timer == nullptr || !have_credentials.load() || receiver_scan_in_flight.load() ||
        station_scan_in_flight.load() || setup_active.load()) return;
    (void)esp_timer_stop(retry_timer);
    const esp_err_t error = esp_timer_start_once(retry_timer, static_cast<uint64_t>(delay_ms) * 1000);
    if (error != ESP_OK) ESP_LOGW(kTag, "Schedule Wi-Fi retry failed: %s", esp_err_to_name(error));
}

void retryCallback(void*) {
    std::lock_guard<std::mutex> lock(station_mutex);
    if (!have_credentials.load() || connected.load() || receiver_scan_in_flight.load() ||
        station_scan_in_flight.load() || setup_active.load()) return;
    const esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "Wi-Fi connect failed: %s", esp_err_to_name(error));
        scheduleRetry(retry_ms.load());
    }
}

void finishReceiverScan(esp_err_t error) {
    receiver_scan_in_flight.store(false);
    if (!receiver_scan_cancelled) {
        receiver_scan.state = error == ESP_OK ? ReceiverScanState::Done : ReceiverScanState::Failed;
        receiver_scan.error = error;
        if (error != ESP_OK) receiver_scan.networks.clear();
        ++receiver_scan.generation;
    }
    receiver_scan_cancelled = false;
    if (!connected.load()) scheduleRetry(100);
}

void collectReceiverScan(const wifi_event_sta_scan_done_t* event) {
    // Unrelated scan events belong to the factory Settings app. Do not consume
    // or clear its driver AP list when we have not started a scan ourselves.
    if (!receiver_scan_in_flight.load()) return;
    esp_err_t error = event != nullptr && event->status == 0 ? ESP_OK : ESP_FAIL;
    uint16_t count = 0;
    wifi_ap_record_t* records = nullptr;
    if (!receiver_scan_cancelled && error == ESP_OK) {
        error = esp_wifi_scan_get_ap_num(&count);
        if (error == ESP_OK && count != 0) {
            records = static_cast<wifi_ap_record_t*>(std::calloc(count, sizeof(wifi_ap_record_t)));
            error = records == nullptr ? ESP_ERR_NO_MEM : esp_wifi_scan_get_ap_records(&count, records);
        }
    }
    if (error == ESP_OK && !receiver_scan_cancelled) {
        for (uint16_t i = 0; records != nullptr && i < count; ++i) {
            const char* ssid = reinterpret_cast<const char*>(records[i].ssid);
            const size_t length = strnlen(ssid, sizeof(records[i].ssid));
            if (length <= sizeof(kReceiverPrefix) - 1 ||
                std::strncmp(ssid, kReceiverPrefix, sizeof(kReceiverPrefix) - 1) != 0) continue;
            const std::string name(ssid, length);
            auto& networks = receiver_scan.networks;
            auto existing = std::find_if(networks.begin(), networks.end(), [&name](const ReceiverNetwork& network) {
                return network.ssid == name;
            });
            if (existing == networks.end()) networks.push_back({name, records[i].rssi});
            else existing->rssi = std::max(existing->rssi, static_cast<int>(records[i].rssi));
            std::sort(networks.begin(), networks.end(), [](const ReceiverNetwork& a, const ReceiverNetwork& b) {
                return a.rssi != b.rssi ? a.rssi > b.rssi : a.ssid < b.ssid;
            });
            if (networks.size() > kMaxReceiverNetworks) networks.resize(kMaxReceiverNetworks);
        }
    }
    std::free(records);
    // get_ap_records normally releases all driver records. This also handles
    // cancellation, empty scans, driver errors and allocation failures.
    (void)esp_wifi_clear_ap_list();
    finishReceiverScan(error);
}

void collectStationScan(const wifi_event_sta_scan_done_t* event) {
    // Sibling of collectReceiverScan: each scan consumer early-returns unless
    // it owns the radio, so exactly one of them consumes the driver list.
    if (!station_scan_in_flight.load()) return;
    esp_err_t error = event != nullptr && event->status == 0 ? ESP_OK : ESP_FAIL;
    uint16_t count = 0;
    wifi_ap_record_t* records = nullptr;
    if (error == ESP_OK) {
        error = esp_wifi_scan_get_ap_num(&count);
        if (error == ESP_OK && count != 0) {
            if (count > 20) count = 20;
            records = static_cast<wifi_ap_record_t*>(std::calloc(count, sizeof(wifi_ap_record_t)));
            error = records == nullptr ? ESP_ERR_NO_MEM : esp_wifi_scan_get_ap_records(&count, records);
        }
    }
    if (error == ESP_OK) {
        std::vector<WifiApNetwork> networks;
        for (uint16_t i = 0; records != nullptr && i < count; ++i) {
            const size_t length = strnlen(reinterpret_cast<const char*>(records[i].ssid),
                                          sizeof(records[i].ssid));
            if (length == 0) continue; // Hidden SSIDs are not selectable.
            networks.push_back({std::string(reinterpret_cast<const char*>(records[i].ssid), length),
                                records[i].rssi, records[i].authmode == WIFI_AUTH_OPEN});
        }
        std::sort(networks.begin(), networks.end(), [](const WifiApNetwork& a, const WifiApNetwork& b) {
            return a.rssi != b.rssi ? a.rssi > b.rssi : a.ssid < b.ssid;
        });
        station_scan.networks = std::move(networks);
        station_scan.state = ReceiverScanState::Done;
    } else {
        station_scan.networks.clear();
        station_scan.state = ReceiverScanState::Failed;
    }
    std::free(records);
    (void)esp_wifi_clear_ap_list();
    station_scan_in_flight.store(false);
    ++station_scan.generation;
    if (!connected.load()) scheduleRetry(100);
}

void eventHandler(void*, esp_event_base_t base, int32_t event_id, void* data) {
    std::lock_guard<std::mutex> lock(station_mutex);
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        scheduleRetry(100);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        collectReceiverScan(static_cast<const wifi_event_sta_scan_done_t*>(data));
        collectStationScan(static_cast<const wifi_event_sta_scan_done_t*>(data));
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        connected.store(false);
        if (receiver_scan_in_flight.load()) finishReceiverScan(ESP_ERR_INVALID_STATE);
        if (station_scan_in_flight.load()) {
            station_scan_in_flight.store(false);
            station_scan.state = ReceiverScanState::Failed;
            station_scan.error = ESP_ERR_INVALID_STATE;
            station_scan.networks.clear();
            ++station_scan.generation;
        }
        if (retry_timer != nullptr) (void)esp_timer_stop(retry_timer);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        connected.store(false);
        const uint32_t current = retry_ms.load();
        scheduleRetry(current);
        retry_ms.store(current >= kMaxRetryMs / 2 ? kMaxRetryMs : current * 2);
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        connected.store(true);
        retry_ms.store(1000);
        if (retry_timer != nullptr) (void)esp_timer_stop(retry_timer);
        ESP_LOGI(kTag, "Wi-Fi connected and IP acquired");
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        connected.store(false);
    }
}

esp_err_t ensureStack() {
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr &&
        esp_netif_create_default_wifi_sta() == nullptr) return ESP_ERR_NO_MEM;

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&config);
    if (error != ESP_OK && error != ESP_ERR_WIFI_INIT_STATE) return error;
    return ESP_OK;
}

// station_mutex is held by the worker. Keep the setup flag set if restoration
// fails, so a subsequent stop call can retry without exposing a stale success.
esp_err_t restoreSetupAccessPoint(std::unique_lock<std::mutex>& station_lock) {
    esp_err_t error = ESP_OK;
    if (setup_mode_changed) {
        // An unconfigured AP can have an empty SSID. The driver's setter need
        // not accept that snapshot; there is no user AP config to restore in
        // that case, and returning to STA disables the temporary AP entirely.
        if (setup_previous_ap_configured) {
            error = esp_wifi_set_config(WIFI_IF_AP, &setup_previous_ap);
            if (error != ESP_OK) return error;
        }
        error = esp_wifi_set_mode(setup_previous_mode);
        if (error != ESP_OK) return error;
        setup_mode_changed = false;
    }
    if (setup_netif != nullptr && (setup_owns_netif || setup_saved_netif)) {
        const esp_err_t dhcp_error = esp_netif_dhcps_stop(setup_netif);
        if (dhcp_error != ESP_OK && dhcp_error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return dhcp_error;
        if (setup_owns_netif) {
            // Default netif teardown unregisters event handlers. Those use
            // the event-loop mutex, while our handler takes station_mutex;
            // never hold both in the reverse order from the worker.
            station_lock.unlock();
            esp_netif_destroy_default_wifi(setup_netif);
            station_lock.lock();
            setup_owns_netif = false;
        } else if (setup_saved_netif) {
            error = esp_netif_set_ip_info(setup_netif, &setup_previous_ip);
            if (error != ESP_OK) return error;
            error = esp_netif_set_dns_info(setup_netif, ESP_NETIF_DNS_MAIN, &setup_previous_dns);
            if (error != ESP_OK) return error;
            error = esp_netif_dhcps_option(setup_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                          &setup_previous_offer_dns, sizeof(setup_previous_offer_dns));
            if (error != ESP_OK) return error;
            if (setup_previous_dhcp != ESP_NETIF_DHCP_STOPPED) {
                error = esp_netif_dhcps_start(setup_netif);
                if (error != ESP_OK && error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) return error;
            }
        }
        setup_netif = nullptr;
    }
    setup_netif = nullptr;
    error = esp_wifi_set_storage(using_receiver.load() ? WIFI_STORAGE_RAM : WIFI_STORAGE_FLASH);
    if (error != ESP_OK) return error;
    setup_saved_netif = false;
    setup_previous_ap_configured = false;
    std::memset(&setup_previous_ap, 0, sizeof(setup_previous_ap));
    std::fill(setup_access_point.password.begin(), setup_access_point.password.end(), '\0');
    setup_access_point = {};
    setup_ready = false;
    setup_active.store(false);
    if (!connected.load()) scheduleRetry(100);
    return ESP_OK;
}
} // namespace

bool station_connected() { return connected.load(); }

esp_err_t start() {
    if (started.load()) return ESP_OK;
    esp_err_t error = ensureStack();
    if (error != ESP_OK) return error;

    error = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (error != ESP_OK) return error;

    wifi_mode_t mode = WIFI_MODE_NULL;
    error = esp_wifi_get_mode(&mode);
    if (error != ESP_OK) return error;
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_NULL) {
        // A previous session (typically factory test firmware) left an AP or
        // APSTA configuration in flash NVS. The made app owns the radio from
        // here on: drop the stray softAP so phone setup works and no unknown
        // hotspot broadcasts. A runtime-created AP is still respected by
        // start_setup_ap()'s guard — this only cleans persisted boot state.
        wifi_config_t empty_ap = {};
        error = esp_wifi_set_config(WIFI_IF_AP, &empty_ap);
        if (error != ESP_OK) return error;
        error = esp_wifi_set_mode(WIFI_MODE_STA);
        if (error != ESP_OK) return error;
        mode = WIFI_MODE_STA;
        ESP_LOGW(kTag, "Cleared persisted AP state from NVS, STA-only");
    }

    wifi_config_t config = {};
    bool apply_config = readSavedConfig(config);
    const char* source = "saved Vibe NVS";
    if (!apply_config) {
        error = esp_wifi_get_config(WIFI_IF_STA, &config);
        if (error != ESP_OK) return error;
        source = "existing Wi-Fi NVS";
        if (!hasDriverConfig(config)) {
            apply_config = validCredentials(CONFIG_VIBE_WIFI_SSID, CONFIG_VIBE_WIFI_PASSWORD);
            if (apply_config) {
                fillConfig(config, CONFIG_VIBE_WIFI_SSID, CONFIG_VIBE_WIFI_PASSWORD);
                source = "local build fallback";
            }
        }
    }

    const bool configured = hasDriverConfig(config);
    if (apply_config) {
        error = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (error != ESP_OK) return error;
    }
    have_credentials.store(configured);
    std::memset(&config, 0, sizeof(config));

    const esp_timer_create_args_t timer_config = {
        .callback = retryCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vibe_wifi_retry",
    };
    error = esp_timer_create(&timer_config, &retry_timer);
    if (error != ESP_OK) return error;
    error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, eventHandler, nullptr);
    if (error != ESP_OK) {
        esp_timer_delete(retry_timer);
        retry_timer = nullptr;
        return error;
    }
    error = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, eventHandler, nullptr);
    if (error != ESP_OK) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, eventHandler);
        esp_timer_delete(retry_timer);
        retry_timer = nullptr;
        return error;
    }

    error = esp_wifi_start();
    if (error != ESP_OK && error != ESP_ERR_WIFI_STATE) {
        esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, eventHandler);
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, eventHandler);
        esp_timer_delete(retry_timer);
        retry_timer = nullptr;
        return error;
    }
    started.store(true);
    if (error == ESP_ERR_WIFI_STATE) scheduleRetry(100);
    if (configured) {
        ESP_LOGI(kTag, "Wi-Fi STA started (%s)", source);
    } else {
        ESP_LOGI(kTag, "Wi-Fi STA started without credentials");
    }
    return ESP_OK;
}

esp_err_t save_credentials(const char* ssid, const char* password) {
    if (!started.load()) return ESP_ERR_INVALID_STATE;
    if (!validCredentials(ssid, password)) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(station_mutex);
    if (using_receiver.load() || setup_active.load()) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_str(handle, "ssid", ssid);
    if (error == ESP_OK) error = nvs_set_str(handle, "password", password);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) return error;

    wifi_config_t config = {};
    fillConfig(config, ssid, password);
    (void)esp_timer_stop(retry_timer);
    (void)esp_wifi_disconnect();
    error = esp_wifi_set_config(WIFI_IF_STA, &config);
    // Erase our temporary stack copy of the password.
    std::memset(&config, 0, sizeof(config));
    if (error != ESP_OK) return error;
    have_credentials.store(true);
    connected.store(false);
    retry_ms.store(1000);
    scheduleRetry(100);
    return ESP_OK;
}

esp_err_t save_receiver_credentials(const char* ssid, const char* password) {
    if (!validReceiverCredentials(ssid, password)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_str(handle, "rx_ssid", ssid);
    if (error == ESP_OK) error = nvs_set_str(handle, "rx_pass", password);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

bool receiver_ssid(char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) return false;
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t required = capacity;
    const bool found = nvs_get_str(handle, "rx_ssid", out, &required) == ESP_OK;
    nvs_close(handle);
    return found;
}

esp_err_t connect_receiver() {
    if (!started.load()) return ESP_ERR_INVALID_STATE;
    wifi_config_t receiver = {};
    if (!readReceiverConfig(receiver)) return ESP_ERR_NOT_FOUND;
    std::lock_guard<std::mutex> lock(station_mutex);
    if (setup_active.load()) {
        std::memset(&receiver, 0, sizeof(receiver));
        return ESP_ERR_INVALID_STATE;
    }
    if (!using_receiver.load()) {
        const esp_err_t error = esp_wifi_get_config(WIFI_IF_STA, &previous_station);
        if (error != ESP_OK) {
            std::memset(&receiver, 0, sizeof(receiver));
            return error;
        }
        previous_had_credentials = hasDriverConfig(previous_station);
    }
    have_credentials.store(false);
    connected.store(false);
    if (retry_timer != nullptr) (void)esp_timer_stop(retry_timer);
    (void)esp_wifi_disconnect();
    esp_err_t error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_STA, &receiver);
    std::memset(&receiver, 0, sizeof(receiver));
    if (error != ESP_OK) {
        (void)esp_wifi_set_config(WIFI_IF_STA, &previous_station);
        (void)esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        using_receiver.store(false);
        have_credentials.store(previous_had_credentials);
        if (previous_had_credentials) scheduleRetry(100);
        std::memset(&previous_station, 0, sizeof(previous_station));
        return error;
    }
    using_receiver.store(true);
    have_credentials.store(true);
    retry_ms.store(1000);
    scheduleRetry(100);
    ESP_LOGI(kTag, "Connecting to saved receiver hotspot");
    return ESP_OK;
}

esp_err_t restore_station() {
    if (!started.load()) return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> lock(station_mutex);
    if (setup_active.load()) return ESP_ERR_INVALID_STATE;
    if (!using_receiver.load()) return ESP_OK;
    have_credentials.store(false);
    connected.store(false);
    if (retry_timer != nullptr) (void)esp_timer_stop(retry_timer);
    (void)esp_wifi_disconnect();
    const esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &previous_station);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Cannot restore previous station: %s", esp_err_to_name(error));
        return error;
    }
    (void)esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    using_receiver.store(false);
    have_credentials.store(previous_had_credentials);
    retry_ms.store(1000);
    if (previous_had_credentials) scheduleRetry(100);
    std::memset(&previous_station, 0, sizeof(previous_station));
    ESP_LOGI(kTag, "Previous Wi-Fi station restored");
    return ESP_OK;
}

bool receiver_active() { return using_receiver.load(); }

esp_err_t request_receiver_scan() {
    if (!started.load()) return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> lock(station_mutex);
    if (setup_active.load()) return ESP_ERR_INVALID_STATE;
    if (station_scan_in_flight.load()) return ESP_ERR_INVALID_STATE;
    if (receiver_scan_in_flight.load()) {
        // Re-entering the page can reuse a scan that was already running when
        // it closed. Repeated refresh taps never restart the radio scan.
        receiver_scan_cancelled = false;
        receiver_scan.state = ReceiverScanState::Scanning;
        receiver_scan.error = ESP_OK;
        ++receiver_scan.generation;
        return ESP_OK;
    }
    receiver_scan.networks.clear();
    receiver_scan.state = ReceiverScanState::Scanning;
    receiver_scan.error = ESP_OK;
    ++receiver_scan.generation;
    receiver_scan_cancelled = false;
    receiver_scan_in_flight.store(true);
    if (retry_timer != nullptr) (void)esp_timer_stop(retry_timer);
    // The driver cannot scan while it is trying to associate. Abort only a
    // pending attempt; never disconnect a station that already has an IP.
    if (!connected.load()) (void)esp_wifi_disconnect();
    wifi_scan_config_t config = {};
    config.show_hidden = false;
    config.channel = 0;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    const esp_err_t error = esp_wifi_scan_start(&config, false);
    if (error != ESP_OK) finishReceiverScan(error);
    return error;
}

ReceiverScanSnapshot receiver_scan_snapshot() {
    std::lock_guard<std::mutex> lock(station_mutex);
    return receiver_scan;
}

esp_err_t request_station_scan() {
    if (!started.load()) return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> lock(station_mutex);
    if (setup_active.load()) return ESP_ERR_INVALID_STATE;
    if (receiver_scan_in_flight.load()) return ESP_ERR_INVALID_STATE;
    if (station_scan_in_flight.load()) {
        // A scan is already owning the radio; the caller just re-requests.
        station_scan.state = ReceiverScanState::Scanning;
        station_scan.error = ESP_OK;
        return ESP_OK;
    }
    station_scan.networks.clear();
    station_scan.state = ReceiverScanState::Scanning;
    station_scan.error = ESP_OK;
    ++station_scan.generation;
    station_scan_in_flight.store(true);
    if (retry_timer != nullptr) (void)esp_timer_stop(retry_timer);
    // The driver cannot scan while it is trying to associate. Abort only a
    // pending attempt; never disconnect a station that already has an IP.
    if (!connected.load()) (void)esp_wifi_disconnect();
    wifi_scan_config_t config = {};
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    const esp_err_t error = esp_wifi_scan_start(&config, false);
    if (error != ESP_OK) {
        station_scan.state = ReceiverScanState::Failed;
        station_scan.error = error;
        station_scan.networks.clear();
        ++station_scan.generation;
        station_scan_in_flight.store(false);
        if (!connected.load()) scheduleRetry(100);
    }
    return error;
}

StationScanSnapshot station_scan_snapshot() {
    std::lock_guard<std::mutex> lock(station_mutex);
    return station_scan;
}

void cancel_receiver_scan() {
    std::lock_guard<std::mutex> lock(station_mutex);
    receiver_scan_cancelled = receiver_scan_in_flight.load();
    receiver_scan.networks.clear();
    receiver_scan.state = ReceiverScanState::Idle;
    receiver_scan.error = ESP_OK;
    ++receiver_scan.generation;
}

esp_err_t start_setup_ap(SetupAccessPoint& out) {
    out = {};
    if (!started.load()) return ESP_ERR_INVALID_STATE;
    std::lock_guard<std::mutex> lifecycle_lock(setup_mutex);
    std::unique_lock<std::mutex> lock(station_mutex);
    if (setup_active.load()) {
        // A failed start may retain cleanup state. Do not report it as a
        // working hotspot; stop_setup_ap() must complete the restoration.
        if (!setup_ready) return ESP_ERR_INVALID_STATE;
        out = setup_access_point;
        return ESP_OK;
    }
    if (receiver_scan_in_flight.load()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = esp_wifi_get_mode(&setup_previous_mode);
    if (error != ESP_OK) return error;
    error = esp_wifi_get_config(WIFI_IF_AP, &setup_previous_ap);
    if (error != ESP_OK) return error;
    setup_previous_ap_configured = setup_previous_ap.ap.ssid_len > 0 || setup_previous_ap.ap.ssid[0] != 0;
    // Respect a provisioning hotspot already owned by the factory desktop:
    // that is an AP interface with a configured SSID, not merely APSTA mode.
    // A persisted APSTA with an empty AP (accidental NVS state) must not make
    // phone setup permanently unavailable.
    if (setup_previous_mode == WIFI_MODE_APSTA && setup_previous_ap_configured) {
        ESP_LOGW(kTag, "Setup AP refused: APSTA with configured AP ssid='%s'",
                 reinterpret_cast<const char*>(setup_previous_ap.ap.ssid));
        return ESP_ERR_INVALID_STATE;
    }
    if (setup_previous_mode != WIFI_MODE_STA && setup_previous_mode != WIFI_MODE_APSTA) {
        ESP_LOGW(kTag, "Setup AP refused: Wi-Fi mode %d is owned elsewhere", (int)setup_previous_mode);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t mac[6] = {};
    error = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (error != ESP_OK) return error;

    setup_active.store(true);
    if (retry_timer != nullptr) (void)esp_timer_stop(retry_timer);
    // Only abort a pending association. An established connection (including
    // the receiver hotspot) stays on its channel while the phone joins APSTA.
    if (!connected.load()) (void)esp_wifi_disconnect();
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) {
        setup_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        setup_owns_netif = setup_netif == nullptr;
        if (setup_owns_netif) {
            // Registration can wait for the event loop. setup_active still
            // gates retries and connection changes while this lock is free.
            lock.unlock();
            setup_netif = esp_netif_create_default_wifi_ap();
            lock.lock();
        }
        if (setup_netif == nullptr) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK && !setup_owns_netif) {
        error = esp_netif_get_ip_info(setup_netif, &setup_previous_ip);
        if (error == ESP_OK) error = esp_netif_dhcps_get_status(setup_netif, &setup_previous_dhcp);
        if (error == ESP_OK) error = esp_netif_get_dns_info(setup_netif, ESP_NETIF_DNS_MAIN, &setup_previous_dns);
        if (error == ESP_OK) error = esp_netif_dhcps_option(setup_netif, ESP_NETIF_OP_GET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                                        &setup_previous_offer_dns, sizeof(setup_previous_offer_dns));
        setup_saved_netif = error == ESP_OK;
    }
    if (error == ESP_OK) {
        error = esp_netif_dhcps_stop(setup_netif);
        if (error == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) error = ESP_OK;
    }
    if (error == ESP_OK) {
        esp_netif_ip_info_t ip = {};
        ip.ip.addr = ESP_IP4TOADDR(192, 168, 8, 1);
        ip.gw.addr = ip.ip.addr;
        ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        error = esp_netif_set_ip_info(setup_netif, &ip);
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ip.ip.addr;
        if (error == ESP_OK) error = esp_netif_set_dns_info(setup_netif, ESP_NETIF_DNS_MAIN, &dns);
        uint8_t offer_dns = 1;
        if (error == ESP_OK) error = esp_netif_dhcps_option(setup_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                                        &offer_dns, sizeof(offer_dns));
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_mode(WIFI_MODE_APSTA);
        setup_mode_changed = error == ESP_OK;
    }
    SetupAccessPoint details;
    if (error == ESP_OK) {
        wifi_config_t ap = {};
        char ssid[24] = {};
        char password[9] = {};
        std::snprintf(ssid, sizeof(ssid), "Made-Setup-%02X%02X", mac[4], mac[5]);
        std::snprintf(password, sizeof(password), "%08lu", static_cast<unsigned long>(esp_random() % 100000000));
        std::memcpy(ap.ap.ssid, ssid, std::strlen(ssid));
        std::memcpy(ap.ap.password, password, 8);
        ap.ap.ssid_len = std::strlen(ssid);
        ap.ap.channel = 6;
        ap.ap.max_connection = 1;
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.pmf_cfg.required = false;
        details.ssid = ssid;
        details.password = password;
        details.address = "http://192.168.8.1";
        error = esp_wifi_set_config(WIFI_IF_AP, &ap);
        std::memset(password, 0, sizeof(password));
        std::memset(&ap, 0, sizeof(ap));
    }
    if (error == ESP_OK) {
        error = esp_netif_dhcps_start(setup_netif);
        if (error == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) error = ESP_OK;
    }
    if (error != ESP_OK) {
        const esp_err_t cleanup = restoreSetupAccessPoint(lock);
        if (cleanup != ESP_OK) ESP_LOGE(kTag, "Phone hotspot cleanup failed: %s", esp_err_to_name(cleanup));
        std::fill(details.password.begin(), details.password.end(), '\0');
        return error;
    }
    setup_access_point = std::move(details);
    setup_ready = true;
    out = setup_access_point;
    ESP_LOGI(kTag, "Phone setup hotspot started");
    return ESP_OK;
}

esp_err_t stop_setup_ap() {
    std::lock_guard<std::mutex> lifecycle_lock(setup_mutex);
    std::unique_lock<std::mutex> lock(station_mutex);
    if (!setup_active.load()) return ESP_OK;
    setup_ready = false;
    const esp_err_t error = restoreSetupAccessPoint(lock);
    if (error == ESP_OK) ESP_LOGI(kTag, "Phone setup hotspot stopped");
    return error;
}

bool setup_ap_active() { return setup_active.load(); }

bool is_connected() {
    return connected.load();
}

} // namespace vibe_wifi
