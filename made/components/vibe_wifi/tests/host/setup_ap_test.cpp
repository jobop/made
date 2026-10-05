#include <cassert>
#include <cctype>
#include <iostream>
#include "fake_idf.hpp"
#include "vibe_wifi.hpp"

int main() {
    using namespace vibe_wifi;
    SetupAccessPoint details;
    assert(start_setup_ap(details) == ESP_ERR_INVALID_STATE && details.ssid.empty());
    assert(start() == ESP_OK);
    assert(save_credentials("Office", "office-pass") == ESP_OK);
    const auto saved = fake::nvs;
    const auto station = fake::config;
    const int writes = fake::writes, persistent_writes = fake::persistent_writes;
    fake::emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    const int disconnects = fake::disconnects;
    assert(start_setup_ap(details) == ESP_OK && setup_ap_active());
    assert(details.ssid == "Made-Setup-EF19" && details.address == "http://192.168.8.1");
    assert(details.password.size() == 8);
    for (char digit : details.password) assert(std::isdigit(static_cast<unsigned char>(digit)));
    assert(fake::mode == WIFI_MODE_APSTA && fake::storage == WIFI_STORAGE_RAM);
    assert(fake::ap_config.ap.max_connection == 1 && fake::ap_config.ap.authmode == WIFI_AUTH_WPA2_PSK);
    assert(fake::ap_netif.ip.ip.addr == ESP_IP4TOADDR(192,168,8,1));
    assert(fake::ap_netif.dns.ip.u_addr.ip4.addr == fake::ap_netif.ip.ip.addr && fake::ap_netif.offer_dns == 1);
    assert(fake::ap_netif.dhcp == ESP_NETIF_DHCP_STARTED);
    assert(fake::disconnects == disconnects && fake::writes == writes && fake::persistent_writes == persistent_writes);
    assert(fake::nvs == saved && std::memcmp(&station, &fake::config, sizeof(station)) == 0);
    SetupAccessPoint repeated;
    assert(start_setup_ap(repeated) == ESP_OK && repeated.password == details.password && fake::ap_creates == 1);
    assert(request_receiver_scan() == ESP_ERR_INVALID_STATE);
    assert(save_credentials("Other", "other-pass") == ESP_ERR_INVALID_STATE && fake::nvs == saved);
    assert(restore_station() == ESP_ERR_INVALID_STATE);
    fake::emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    const int connects = fake::connects;
    fake::timer_callback(nullptr);
    assert(!fake::timer_active && fake::connects == connects);
    assert(stop_setup_ap() == ESP_OK && !setup_ap_active());
    assert(fake::mode == WIFI_MODE_STA && fake::storage == WIFI_STORAGE_FLASH);
    assert(!fake::ap_exists && fake::ap_destroys == 1 && fake::timer_active);
    assert(stop_setup_ap() == ESP_OK && fake::ap_destroys == 1);

    // Never replace another app's running AP or overlap an active radio scan.
    fake::mode = WIFI_MODE_APSTA;
    assert(start_setup_ap(repeated) == ESP_ERR_INVALID_STATE && !setup_ap_active());
    fake::mode = WIFI_MODE_STA;
    assert(request_receiver_scan() == ESP_OK);
    assert(start_setup_ap(repeated) == ESP_ERR_INVALID_STATE);
    cancel_receiver_scan();
    fake::done();

    // Preserve an existing but inactive AP interface, its DNS/IP/DHCP policy,
    // and factory AP config. No temporary configuration may be persisted.
    fake::ap_exists = true;
    fake::ap_netif.ip.ip.addr = ESP_IP4TOADDR(10,2,3,1);
    fake::ap_netif.ip.gw.addr = fake::ap_netif.ip.ip.addr;
    fake::ap_netif.dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1,2,3,4);
    fake::ap_netif.offer_dns = 0;
    fake::ap_netif.dhcp = ESP_NETIF_DHCP_STOPPED;
    std::memcpy(fake::ap_config.ap.ssid, "Factory AP", 10);
    const auto original_ap = fake::ap_config;
    const auto original_netif = fake::ap_netif;
    assert(start_setup_ap(repeated) == ESP_OK && repeated.password != details.password);
    assert(stop_setup_ap() == ESP_OK);
    assert(fake::ap_exists && fake::ap_destroys == 1);
    assert(std::memcmp(&original_ap, &fake::ap_config, sizeof(original_ap)) == 0);
    assert(fake::ap_netif.ip.ip.addr == original_netif.ip.ip.addr);
    assert(fake::ap_netif.dns.ip.u_addr.ip4.addr == original_netif.dns.ip.u_addr.ip4.addr);
    assert(fake::ap_netif.offer_dns == 0 && fake::ap_netif.dhcp == ESP_NETIF_DHCP_STOPPED);
    assert(fake::nvs == saved && fake::persistent_writes == persistent_writes);

    // Roll back failures during AP config / DHCP startup and permit retry.
    fake::ap_exists = false;
    fake::fail_ap_config_once = 1;
    assert(start_setup_ap(repeated) == ESP_FAIL && repeated.ssid.empty());
    assert(!setup_ap_active() && fake::mode == WIFI_MODE_STA && !fake::ap_exists && fake::timer_active);
    fake::fail_dhcp_once = 1;
    assert(start_setup_ap(repeated) == ESP_FAIL && !setup_ap_active());
    assert(fake::mode == WIFI_MODE_STA && fake::storage == WIFI_STORAGE_FLASH);
    assert(start_setup_ap(repeated) == ESP_OK);
    fake::fail_mode_once = 1;
    assert(stop_setup_ap() == ESP_FAIL && setup_ap_active());
    assert(start_setup_ap(repeated) == ESP_ERR_INVALID_STATE && repeated.ssid.empty());
    assert(stop_setup_ap() == ESP_OK && !setup_ap_active());

    // Receiver station credentials stay transient and its original station is
    // still restored correctly after a phone setup session.
    assert(save_receiver_credentials("VibeReceiver-Test", "12345678") == ESP_OK);
    assert(connect_receiver() == ESP_OK && receiver_active());
    const auto receiver = fake::config;
    fake::emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    assert(start_setup_ap(repeated) == ESP_OK && receiver_active());
    assert(connect_receiver() == ESP_ERR_INVALID_STATE);
    assert(stop_setup_ap() == ESP_OK && receiver_active());
    assert(fake::storage == WIFI_STORAGE_RAM && std::memcmp(&receiver, &fake::config, sizeof(receiver)) == 0);
    assert(restore_station() == ESP_OK && !receiver_active());
    assert(std::memcmp(&station, &fake::config, sizeof(station)) == 0);
    assert(fake::storage == WIFI_STORAGE_FLASH && fake::persistent_writes == persistent_writes);
    fake::ap_exists = false;
    fake::simulate_netif_events = true;
    assert(start_setup_ap(repeated) == ESP_OK);
    assert(stop_setup_ap() == ESP_OK && !setup_ap_active());
    std::cout << "phone setup hotspot lifecycle checks passed\n";
}
