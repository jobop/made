#include "mock.hpp"
#include "../../vibe_pairing.cpp"

using namespace vibe_pairing;

std::string modern(const std::string &id, const std::string &name, bool open = true,
                   int port = 8788)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "VIBE_BRIDGE_V3");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddNumberToObject(root, "port", port);
    cJSON_AddStringToObject(root, "bridgeId", id.c_str());
    cJSON_AddStringToObject(root, "name", name.c_str());
    cJSON_AddBoolToObject(root, "pairingOpen", open);
    char *text = cJSON_PrintUnformatted(root);
    std::string result(text);
    cJSON_free(text);
    cJSON_Delete(root);
    return result;
}

std::string sign(const std::string &token, const std::string &text)
{
    unsigned char key[32], digest[32];
    for (size_t i = 0; i < 32; ++i)
        key[i] = static_cast<unsigned char>(std::stoi(token.substr(i * 2, 2), nullptr, 16));
    test_hmac(key, sizeof(key), text.data(), text.size(), digest);
    std::string result;
    char hex[3];
    for (unsigned char value : digest) { std::snprintf(hex, sizeof(hex), "%02x", value); result += hex; }
    return result;
}

void reply_signed(const std::string &request, const std::string &id, const std::string &token,
                  const std::string &address)
{
    const std::string prefix = "VIBE_DISCOVER_PAIRED_V2 ";
    if (request.rfind(prefix, 0) != 0) return;
    const std::string rest = request.substr(prefix.size());
    const auto separator = rest.find(' ');
    const std::string device = rest.substr(0, separator), challenge = rest.substr(separator + 1);
    const std::string mac = sign(token, "VIBE_BRIDGE_PAIRED_V2\n" + device + "\n" + challenge +
        "\n8788\n" + id + "\n" + address);
    packets.push_back({address, "VIBE_BRIDGE_PAIRED_V2 8788 " + id + " " + address + " " + mac});
}

void reset_counters()
{
    packets.clear(); udp_requests.clear(); http_urls.clear();
    network_http_calls = 0; sockets_opened = sockets_closed = 0;
    on_discovery = {}; during_receive = {}; during_http = {}; http_response = {};
    socket_available = true;
}

void request_scan()
{
    reset_counters();
    assert(use_automatic_discovery());
    assert(network_http_calls == 0 && udp_requests.empty());
}

int main()
{
    initialize();
    begin_foreground_connection();
    request_scan();
    on_discovery = [](const std::string &request) {
        if (request == "VIBE_DISCOVER_V1") {
            packets.push_back({"192.168.1.20", "VIBE_BRIDGE_V1 8788 pc_b"});
            packets.push_back({"192.168.1.30", "VIBE_BRIDGE_V1 8788 legacy"});
        } else if (request == "VIBE_DISCOVER_V3") {
            packets.push_back({"192.168.1.20", modern("pc_b", "B Computer", false)});
            packets.push_back({"192.168.1.10", modern("pc_a", "A Computer")});
            packets.push_back({"192.168.1.11", modern("pc_a", "A Computer")});
        }
    };
    const uint64_t before = mock_clock;
    tick();
    auto found = snapshot();
    assert(found.phase == Phase::ChoosingBridge && !found.scanning);
    assert(found.bridges.size() == 3);
    assert(found.bridges[0].id == "pc_a" && found.bridges[0].url == "http://192.168.1.10:8788");
    assert(found.bridges[1].name == "B Computer" && !found.bridges[1].pairing_open);
    assert(found.bridges[2].id == "legacy");
    assert(mock_clock - before <= 1700000);
    assert(udp_requests.size() == 4 && sockets_opened == sockets_closed);
    assert(network_http_calls == 0);
    mock_clock += 100000000;
    tick();
    assert(udp_requests.size() == 4 && network_http_calls == 0);
    assert(!select_bridge("other", found.bridges[0].url));
    assert(!select_bridge(found.bridges[0].id, "http://evil.local:8788"));
    assert(select_bridge(found.bridges[1].id, found.bridges[1].url));
    assert(!select_bridge(found.bridges[0].id, found.bridges[0].url));
    assert(snapshot().phase == Phase::Discovering && network_http_calls == 0);
    tick();
    assert(snapshot().phase == Phase::WaitingApproval);
    assert(http_urls.size() == 1 && http_urls[0] == "http://192.168.1.20:8788/pair/request");
    std::cout << "PASS: bounded multi-PC/V1/V3 scan, stable choices, only tapped PC receives a request\n";

    request_scan();
    on_discovery = [](const std::string &request) {
        if (request == "VIBE_DISCOVER_V3") packets.push_back({"192.168.1.42", modern("only", "唯一电脑")});
    };
    tick();
    assert(snapshot().phase == Phase::ChoosingBridge && snapshot().bridges.size() == 1);
    assert(network_http_calls == 0);
    scan_bridges();
    assert(snapshot().scanning && !select_bridge("only", "http://192.168.1.42:8788"));
    on_discovery = {};
    tick();
    assert(snapshot().phase == Phase::ChoosingBridge && snapshot().bridges.empty() && !snapshot().scanning);
    tick();
    assert(network_http_calls == 0);
    std::cout << "PASS: one result still requires selection; refresh invalidates rows; empty results remain selectable-screen state\n";

    request_scan();
    on_discovery = [](const std::string &request) {
        if (request != "VIBE_DISCOVER_V3") return;
        packets.push_back({"192.168.1.40", modern("ok", "办公室电脑")});
        packets.push_back({"192.168.1.41", modern("bad", "name\nforged")});
        packets.push_back({"192.168.1.42", modern("bad", std::string(97, 'a'))});
        packets.push_back({"192.168.1.43", modern("bad", std::string("\xc0\xaf", 2))});
        packets.push_back({"192.168.1.44", modern("bad/id", "invalid ID")});
        packets.push_back({"192.168.1.45", modern("bad", "invalid port", true, 65536)});
        packets.push_back({"192.168.1.46", modern("bad", "trailing") + " garbage"});
        packets.push_back({"192.168.1.47", "VIBE_BRIDGE_V1 8788 pc_id extra"});
        std::string nul = modern("nul", "NUL_TOKEN");
        nul.replace(nul.find("NUL_TOKEN"), 9, "Valid\\u0000Hidden");
        packets.push_back({"192.168.1.49", nul});
        packets.push_back({"224.1.1.1", modern("multicast", "multicast sender")});
        std::string spoof = modern("safe", "Safe sender");
        spoof.insert(spoof.size() - 1, ",\"url\":\"http://evil.example\"");
        packets.push_back({"192.168.1.48", spoof});
    };
    tick();
    found = snapshot();
    assert(found.bridges.size() == 2 && network_http_calls == 0);
    auto safe = std::find_if(found.bridges.begin(), found.bridges.end(), [](const Bridge &b) { return b.id == "safe"; });
    assert(safe != found.bridges.end() && safe->url == "http://192.168.1.48:8788");
    std::cout << "PASS: malformed metadata/control characters/invalid UTF-8/ports dropped; endpoint always uses sender IP\n";

    request_scan();
    on_discovery = [](const std::string &request) {
        if (request != "VIBE_DISCOVER_V3") return;
        for (int i = 1; i <= 50; ++i)
            packets.push_back({"192.168.1." + std::to_string(i), modern("pc" + std::to_string(i), "Computer " + std::to_string(i))});
    };
    tick();
    assert(snapshot().bridges.size() == 12 && sockets_opened == sockets_closed && network_http_calls == 0);
    std::cout << "PASS: discovery bounds retained rows to 12 under duplicate/large reply sets\n";

    const auto old = snapshot().bridges.front();
    assert(select_bridge(old.id, old.url));
    suspend_foreground();
    tick();
    assert(network_http_calls == 0);
    assert(!select_bridge(old.id, old.url));
    begin_foreground_connection();
    on_discovery = {};
    tick();
    assert(snapshot().phase == Phase::ChoosingBridge && snapshot().bridges.empty() && network_http_calls == 0);
    request_scan();
    during_receive = [] { suspend_foreground(); };
    tick();
    assert(!authorized_for_foreground() && sockets_opened == sockets_closed && network_http_calls == 0);
    assert(!select_bridge(old.id, old.url));
    std::cout << "PASS: pending selection and in-progress scan cannot survive app exit/reopen\n";

    const std::string token(64, 'a');
    nvs["token"] = token; nvs["bridge"] = "pc_a";
    begin_foreground_connection();
    reset_counters();
    on_discovery = [&](const std::string &request) { reply_signed(request, "pc_a", token, "192.168.1.10"); };
    tick();
    assert(snapshot().phase == Phase::Paired && authorized_for_foreground());
    assert(network_http_calls == 0 && udp_requests.size() == 1);
    request_scan();
    on_discovery = [&](const std::string &request) {
        reply_signed(request, "pc_a", token, "192.168.1.10");
        if (request == "VIBE_DISCOVER_V3") {
            packets.push_back({"192.168.1.10", modern("pc_a", "A Saved")});
            packets.push_back({"192.168.1.20", modern("pc_b", "B New")});
        }
    };
    tick();
    assert(snapshot().phase == Phase::ChoosingBridge && snapshot().bridges.size() == 2);
    assert(!authorized_for_foreground() && network_http_calls == 0);
    for (const auto &request : udp_requests) assert(request.rfind("VIBE_DISCOVER_PAIRED", 0) != 0);
    assert(select_bridge("pc_b", "http://192.168.1.20:8788"));
    http_response = [](const std::string &) { return std::make_pair(403, std::string("{}")); };
    tick();
    assert(snapshot().phase == Phase::Error && !authorized_for_foreground());
    assert(snapshot().url == "http://192.168.1.20:8788" && snapshot().bridge_id == "pc_b");
    mock_clock += 6000000;
    tick();
    assert(http_urls.size() == 2);
    for (const auto &url : http_urls) assert(url == "http://192.168.1.20:8788/pair/request");
    assert(nvs["token"] == token && nvs["bridge"] == "pc_a");
    std::cout << "PASS: signed saved-PC reconnect retained, explicit Wi-Fi save chooses anew, failed chosen target never reverts to other PC\n";

    begin_foreground_connection();
    reset_counters();
    on_discovery = [&](const std::string &request) {
        // Same ID with the wrong key must not restore authorization.
        reply_signed(request, "pc_a", std::string(64, 'b'), "192.168.1.10");
        if (request == "VIBE_DISCOVER_V3") packets.push_back({"192.168.1.20", modern("pc_b", "B New")});
    };
    tick();
    assert(snapshot().phase == Phase::ChoosingBridge && snapshot().bridges.size() == 1);
    assert(!authorized_for_foreground() && network_http_calls == 0);
    assert(select_bridge("pc_b", "http://192.168.1.20:8788"));
    assert(use_usb_direct());
    const int prior_calls = network_http_calls;
    tick();
    assert(snapshot().access_mode == AccessMode::UsbDirect && network_http_calls == prior_calls);
    assert(!select_bridge("pc_b", "http://192.168.1.20:8788"));
    std::cout << "PASS: invalid saved-PC signatures fall back only to chooser; cross-mode queued selections are discarded\n";

    request_scan();
    on_discovery = [](const std::string &request) {
        if (request == "VIBE_DISCOVER_V3") packets.push_back({"192.168.1.10", modern("pc_a", "A Saved")});
    };
    tick();
    assert(select_bridge("pc_a", "http://192.168.1.10:8788"));
    during_http = [] { suspend_foreground(); };
    tick();
    assert(!authorized_for_foreground() && http_urls.size() == 1);
    assert(http_urls[0].find("/pair/verify?") != std::string::npos);
    assert(snapshot().phase != Phase::WaitingApproval);
    std::cout << "PASS: app exit during selected-PC verification cannot start a follow-up pairing request\n";
    DiscoveredBridge legacy;
    assert(parse_discovered_bridge("VIBE_BRIDGE_V1 8788 pc_old", "192.168.1.4", legacy));
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        state.bridges = {legacy.bridge, {"named", "电脑 mine", "http://192.168.1.5:8788", true}};
    }
    assert(vibe_i18n::set_locale("en"));
    assert(snapshot().bridges[0].name == "Computer 192.168.1.4");
    assert(snapshot().bridges[1].name == "电脑 mine");
    assert(vibe_i18n::set_locale("zh-CN"));
    assert(snapshot().bridges[0].name == "电脑 192.168.1.4");
    std::cout << "PASS: legacy generated bridge names localize; custom PC names remain unchanged\n";
    std::cout << "9 chooser scenarios passed\n";
}
