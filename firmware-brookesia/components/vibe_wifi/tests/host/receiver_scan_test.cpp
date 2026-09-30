#include <cassert>
#include <iostream>
#include "fake_idf.hpp"
#include "vibe_wifi.hpp"

int main() {
    using namespace vibe_wifi;
    assert(request_receiver_scan() == ESP_ERR_INVALID_STATE);
    assert(start() == ESP_OK);
    assert(save_credentials("Office", "office-pass") == ESP_OK);
    const auto saved = fake::nvs;
    const int writes = fake::writes;
    fake::emit(IP_EVENT, IP_EVENT_STA_GOT_IP);
    const int disconnects = fake::disconnects;
    assert(request_receiver_scan() == ESP_OK);
    assert(receiver_scan_snapshot().state == ReceiverScanState::Scanning);
    assert(!fake::scan_blocked && !fake::scan_config.show_hidden && fake::scan_config.channel == 0);
    assert(!fake::timer_active && fake::disconnects == disconnects);
    assert(fake::writes == writes && fake::nvs == saved);
    fake::records = {fake::ap("Office", -12), fake::ap("VibeReceiver-A", -70),
                     fake::ap("VibeReceiver-B", -30), fake::ap("VibeReceiver-A", -20),
                     fake::ap("VibeReceiver-", -10)};
    fake::done();
    auto result = receiver_scan_snapshot();
    assert(result.state == ReceiverScanState::Done && result.error == ESP_OK);
    assert(result.networks.size() == 2 && result.networks[0].ssid == "VibeReceiver-A");
    assert(result.networks[0].rssi == -20 && result.networks[1].ssid == "VibeReceiver-B");
    assert(fake::records.empty() && !fake::timer_active);

    // A scan started by another app must retain its results for that app.
    const int clears = fake::clear_calls;
    fake::records = {fake::ap("Someone else's scan", -50)};
    fake::done();
    assert(fake::records.size() == 1 && fake::clear_calls == clears);

    // A lost connection must not restart association while scanning. Exiting
    // the page detaches it, then SCAN_DONE releases the driver list and retries.
    fake::emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    assert(fake::timer_active);
    assert(request_receiver_scan() == ESP_OK);
    const int connects = fake::connects;
    fake::timer_callback(nullptr);
    assert(fake::connects == connects && !fake::timer_active);
    cancel_receiver_scan();
    assert(receiver_scan_snapshot().state == ReceiverScanState::Idle);
    const int reads = fake::record_calls;
    fake::records = {fake::ap("VibeReceiver-A", -25)};
    fake::done();
    assert(fake::record_calls == reads && fake::records.empty() && fake::timer_active);
    assert(receiver_scan_snapshot().state == ReceiverScanState::Idle);
    fake::timer_callback(nullptr);
    assert(fake::connects == connects + 1);

    // Repeated refresh and leaving/re-entering do not start competing scans.
    assert(request_receiver_scan() == ESP_OK);
    const int scans = fake::scans;
    cancel_receiver_scan();
    assert(request_receiver_scan() == ESP_OK && fake::scans == scans);
    assert(request_receiver_scan() == ESP_OK && fake::scans == scans);
    fake::records.clear();
    for (int i = 0; i < 20; ++i) {
        fake::records.push_back(fake::ap(("VibeReceiver-" + std::to_string(i)).c_str(), -80 + i));
    }
    fake::done();
    result = receiver_scan_snapshot();
    assert(result.state == ReceiverScanState::Done && result.networks.size() == 12);
    assert(result.networks.front().rssi == -61 && result.networks.back().rssi == -72);
    assert(fake::records.empty());

    // Both start failures and result failures restore the retry schedule.
    fake::scan_error = ESP_FAIL;
    assert(request_receiver_scan() == ESP_FAIL && fake::timer_active);
    assert(receiver_scan_snapshot().state == ReceiverScanState::Failed);
    fake::scan_error = ESP_OK;
    assert(request_receiver_scan() == ESP_OK);
    fake::records_error = ESP_FAIL;
    fake::records = {fake::ap("VibeReceiver-A", -25)};
    fake::done();
    assert(receiver_scan_snapshot().state == ReceiverScanState::Failed);
    assert(fake::records.empty() && fake::timer_active);
    assert(fake::writes == writes && fake::nvs == saved);

    assert(request_receiver_scan() == ESP_OK);
    fake::emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    assert(receiver_scan_snapshot().state == ReceiverScanState::Failed && !fake::timer_active);
    std::cout << "receiver scan lifecycle checks passed\n";
}
