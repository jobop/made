// SPDX-License-Identifier: Apache-2.0
#include "phone_setup_protocol.hpp"
#include "phone_setup_socket.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
using namespace vibe_phone_setup;
using namespace vibe_phone_setup::detail;
namespace {
const std::string nonce = "0123456789abcdef0123456789abcdef";
unsigned checks = 0;
ValidationError parse(const std::string &fields, Submission &out) {
    return parse_submission("{\"nonce\":\"" + nonce + "\"," + fields + "}", nonce, {}, out);
}
void succeeds(const std::string &fields) {
    Submission out;
    assert(parse(fields, out) == ValidationError::None);
    ++checks;
}
void fails(const std::string &fields, ValidationError expected) {
    Submission out;
    out.mode = "unchanged";
    const auto actual = parse(fields, out);
    if (actual != expected) std::cerr << "Case " << checks + 1 << ": expected " << static_cast<int>(expected) << " got " << static_cast<int>(actual) << "\n";
    assert(actual == expected);
    assert(out.mode == "unchanged");
    ++checks;
}
}
int main(int argc, char **argv) {
    // Real esp_http_server's dual-stack listener returns mapped IPv6 for IPv4.
    sockaddr_in ap4{};
    ap4.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "192.168.8.1", &ap4.sin_addr) == 1);
    assert(is_setup_address(reinterpret_cast<const sockaddr *>(&ap4), sizeof(ap4))); ++checks;
    assert(!is_setup_address(reinterpret_cast<const sockaddr *>(&ap4), sizeof(ap4) - 1)); ++checks;
    assert(inet_pton(AF_INET, "192.168.1.2", &ap4.sin_addr) == 1);
    assert(!is_setup_address(reinterpret_cast<const sockaddr *>(&ap4), sizeof(ap4))); ++checks;
    sockaddr_in6 ap6{};
    ap6.sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "::ffff:192.168.8.1", &ap6.sin6_addr) == 1);
    assert(is_setup_address(reinterpret_cast<const sockaddr *>(&ap6), sizeof(ap6))); ++checks;
    for (socklen_t length = 0; length < sizeof(ap6); ++length) {
        assert(!is_setup_address(reinterpret_cast<const sockaddr *>(&ap6), length)); ++checks;
    }
    for (const auto ip : {"::ffff:192.168.1.2", "::ffff:192.168.8.2", "::192.168.8.1", "::1", "fe80::1", "2001:db8::1"}) {
        assert(inet_pton(AF_INET6, ip, &ap6.sin6_addr) == 1);
        assert(!is_setup_address(reinterpret_cast<const sockaddr *>(&ap6), sizeof(ap6))); ++checks;
    }
    assert(!is_setup_address(nullptr, sizeof(sockaddr_storage))); ++checks;
    sockaddr unknown{}; unknown.sa_family = AF_UNSPEC;
    assert(!is_setup_address(&unknown, sizeof(unknown))); ++checks;
    succeeds("\"mode\":\"automatic\"");
    succeeds("\"mode\":\"automatic\",\"wifi_ssid\":\"办公室\",\"wifi_password\":\"testpass123\"");
    succeeds("\"mode\":\"automatic\",\"wifi_ssid\":\"open-wifi\",\"wifi_password\":\"\"");
    succeeds("\"mode\":\"receiver\",\"receiver_ssid\":\"VibeReceiver-TEST\",\"receiver_password\":\"12345678\"");
    succeeds("\"mode\":\"receiver\",\"receiver_ssid\":\"old receiver\",\"receiver_password\":\"legacy-pass!\"");
    fails("\"mode\":\"receiver\",\"receiver_ssid\":\"VibeReceiver-TEST\"", ValidationError::InvalidReceiver);
    fails("\"mode\":\"receiver\",\"receiver_ssid\":\"test\",\"receiver_password\":\"1234567\"", ValidationError::InvalidReceiver);
    for (auto key : {"apiKey", "token", "pairing_token", "command"})
        fails("\"mode\":\"automatic\",\"" + std::string(key) + "\":\"test\"", ValidationError::UnknownField);
    fails("\"mode\":\"automatic\",\"mode\":\"receiver\"", ValidationError::UnknownField);
    fails("\"mode\":\"usb\"", ValidationError::InvalidMode);
    fails("\"mode\":null", ValidationError::InvalidMode);
    fails("\"mode\":\"automatic\",\"nonce\":\"wrong\"", ValidationError::UnknownField);
    fails("\"mode\":\"automatic\",\"wifi_ssid\":\"wifi\",\"wifi_password\":\"short\"", ValidationError::InvalidWifi);
    fails("\"mode\":\"automatic\",\"wifi_password\":\"testpass123\"", ValidationError::InvalidWifi);
    fails("\"mode\":\"automatic\",\"wifi_ssid\":\"" + std::string(33, 's') + "\"", ValidationError::InvalidWifi);
    fails("\"mode\":\"automatic\",\"wifi_ssid\":\"wifi\",\"wifi_password\":\"" + std::string(64, 'p') + "\"", ValidationError::InvalidWifi);
    fails("\"mode\":\"automatic\",\"wifi_ssid\":\"line\\nnext\"", ValidationError::InvalidWifi);
    fails("\"mode\":\"automatic\",\"wifi_ssid\":\"wifi\\u0000evil\"", ValidationError::InvalidJson);
    succeeds("\"mode\":\"automatic\",\"wifi_ssid\":\"literal\\\\u0000\"");
    fails("\"mode\":\"automatic\",\"wifi_ssid\":\"\xc0\xaf\"", ValidationError::InvalidWifi);
    fails("\"mode\":\"automatic\",\"wifi_ssid\":\"\xed\xa0\x80\"", ValidationError::InvalidWifi);
    fails("\"mode\":\"automatic\",\"wifi_ssid\":{}", ValidationError::InvalidJson);
    fails("\"mode\":\"automatic\",\"wifi_ssid\":[1]", ValidationError::InvalidJson);
    succeeds("\"mode\":\"automatic\",\"wifi_ssid\":\"braces{[inside]}\"");
    fails("\"mode\":\"manual\",\"host\":\"http://public.example.com\"", ValidationError::PublicHttp);
    for (auto host : {"https://name.example/path", "https://user@name.example", "https://name.example?q=x", "https://name.example#frag", "https://-bad.example", "https://bad..example", "http://999.168.1.1", "https://[::1]"})
        fails("\"mode\":\"manual\",\"host\":\"" + std::string(host) + "\"", ValidationError::InvalidHost);
    for (auto port : {"0", "65536", "-1", "443.5", "\"443\"", "null", "1e300"})
        fails("\"mode\":\"manual\",\"host\":\"local.example\",\"port\":" + std::string(port), ValidationError::InvalidPort);
    Submission out;
    assert(parse("\"mode\":\"manual\",\"host\":\" HTTPS://Demo.EXAMPLE:8443/ \",\"port\":8788,\"https\":false", out) == ValidationError::None);
    assert(out.host == "demo.example" && out.port == 8443 && out.https); ++checks;
    assert(parse("\"mode\":\"manual\",\"host\":\"http://192.168.1.10\",\"port\":8788", out) == ValidationError::None);
    assert(out.host == "192.168.1.10" && out.port == 8788 && !out.https); ++checks;
    assert(parse("\"mode\":\"manual\",\"host\":\"http://192.168.4.1:8788/\",\"port\":443", out) == ValidationError::None);
    assert(out.port == 8788 && !out.https); ++checks;
    assert(parse("\"mode\":\"manual\",\"host\":\"http://bridge.local/\"", out) == ValidationError::None);
    assert(out.port == 80); ++checks;
    assert(parse("\"mode\":\"manual\",\"host\":\"https://bridge.example/\"", out) == ValidationError::None);
    assert(out.port == 443 && out.https); ++checks;
    const std::string request = "{\"nonce\":\"" + nonce + "\",\"mode\":\"automatic\"}";
    assert(parse_submission(request, std::string(32, 'f'), {}, out) == ValidationError::InvalidNonce); ++checks;
    assert(parse_submission(request + "x", nonce, {}, out) == ValidationError::InvalidJson); ++checks;
    assert(parse_submission(request + std::string(2048, ' '), nonce, {}, out) == ValidationError::InvalidJson); ++checks;
    assert(parse_submission(std::string(990, '[') + "0" + std::string(990, ']'), nonce, {}, out) == ValidationError::InvalidJson); ++checks;
    assert(parse_submission(request.substr(0, 20) + '\0' + request.substr(20), nonce, {}, out) == ValidationError::InvalidJson); ++checks;
    assert(json_content_type("application/json") && json_content_type("Application/JSON; charset=utf-8")); ++checks;
    assert(!json_content_type("text/plain") && !json_content_type("application/json-evil") && !json_content_type("application/json; charset=latin1")); ++checks;
    assert(qr_escape("a:b;c,d\\e\"f") == "a\\:b\\;c\\,d\\\\e\\\"f"); ++checks;
    assert(json_for_script("</script>&") == "\\u003c/script\\u003e\\u0026"); ++checks;
    std::vector<uint8_t> dns = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,3,'a','p','p',4,'t','e','s','t',0,0,1,0,1};
    uint8_t reply[512]{};
    assert(dns_reply(dns.data(), dns.size(), reply, sizeof(reply)) == dns.size() + 16);
    assert(reply[0] == 0x12 && reply[1] == 0x34 && reply[7] == 1);
    assert(std::memcmp(reply + dns.size() + 12, "\xc0\xa8\x08\x01", 4) == 0); ++checks;
    dns[23] = 28;
    assert(dns_reply(dns.data(), dns.size(), reply, sizeof(reply)) == dns.size() && reply[7] == 0); ++checks;
    dns[12] = 0xc0;
    assert(dns_reply(dns.data(), dns.size(), reply, sizeof(reply)) == 0); ++checks;
    dns[12] = 3; dns[5] = 2;
    assert(dns_reply(dns.data(), dns.size(), reply, sizeof(reply)) == 0); ++checks;
    dns[5] = 1;
    assert(dns_reply(dns.data(), dns.size() - 1, reply, sizeof(reply)) == 0); ++checks;
    assert(dns_reply(dns.data(), dns.size(), reply, 10) == 0); ++checks;
    InitialConfig initial;
    initial.wifi_ssid = "</script><script>alert(1)</script>";
    initial.receiver_ssid = "VibeReceiver-DEMO";
    auto html = render_page(false, initial, nonce);
    assert(html.find("__MADE_SETUP_CONFIG__") == std::string::npos);
    assert(html.find(initial.wifi_ssid) == std::string::npos);
    assert(html.find("\\u003c/script\\u003e") != std::string::npos);
    assert(html.find("\"wifi_password\"") == std::string::npos && html.find("\"receiver_password\"") == std::string::npos); ++checks;
    if (argc > 1) {
        initial.wifi_ssid = "Office-demo";
        initial.host = "bridge.example.com";
        std::ofstream preview(argv[1]);
        preview << render_page(argc > 2 && std::strcmp(argv[2], "en") == 0, initial, nonce);
        assert(preview.good());
    }
    std::cout << "PASS: " << checks << " phone setup protocol/escaping/DNS checks\n";
}
