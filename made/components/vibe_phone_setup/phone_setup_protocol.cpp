// SPDX-License-Identifier: Apache-2.0
#include "phone_setup_protocol.hpp"
#include "phone_setup_page.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include "cJSON.h"

namespace vibe_phone_setup::detail {
namespace {
bool safe_text(const std::string &s, size_t maximum) {
    if (s.size() > maximum) return false;
    // Reject malformed UTF-8, controls and Unicode surrogates before storing SSIDs.
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = static_cast<uint8_t>(s[i++]);
        if (cp < 0x20 || cp == 0x7f) return false;
        if (cp < 0x80) continue;
        unsigned following;
        uint32_t minimum;
        if (cp >= 0xc2 && cp <= 0xdf) { following = 1; minimum = 0x80; cp &= 0x1f; }
        else if (cp >= 0xe0 && cp <= 0xef) { following = 2; minimum = 0x800; cp &= 0x0f; }
        else if (cp >= 0xf0 && cp <= 0xf4) { following = 3; minimum = 0x10000; cp &= 7; }
        else return false;
        if (i + following > s.size()) return false;
        while (following--) {
            const uint8_t next = static_cast<uint8_t>(s[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

bool same_nonce(const std::string &a, const std::string &b) {
    if (a.size() != 32 || b.size() != 32) return false;
    unsigned different = 0;
    for (size_t i = 0; i < 32; ++i) different |= static_cast<unsigned char>(a[i] ^ b[i]);
    return different == 0;
}

bool nul_escape(const std::string &s) {
    // cJSON strings cannot represent embedded NULs; reject them before parsing.
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\0') return true;
        if (s[i] != '\\') continue;
        if (s.compare(i, 6, "\\u0000") == 0) return true;
        ++i; // an escaped slash does not begin another escape
    }
    return false;
}

bool flat_json(const std::string &s) {
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted && c == '\\') { ++i; continue; }
        if (c == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (c == '{' || c == '[') {
            // The schema is a flat object. Bound cJSON recursion before parsing.
            if (++depth > 1) return false;
        } else if (c == '}' || c == ']') {
            if (!depth) return false;
            --depth;
        }
    }
    return !quoted && depth == 0;
}

bool string_field(const cJSON *root, const char *key, std::string &out) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!value) { out.clear(); return true; }
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    out = value->valuestring;
    return true;
}

bool valid_port(const std::string &s, uint16_t &out) {
    if (s.empty() || s.size() > 5) return false;
    unsigned port = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        port = port * 10 + (c - '0');
    }
    if (!port || port > 65535) return false;
    out = static_cast<uint16_t>(port);
    return true;
}

bool ipv4(const std::string &host, uint32_t &result) {
    unsigned octet = 0, digits = 0, segments = 0;
    result = 0;
    for (size_t i = 0; i <= host.size(); ++i) {
        const char c = i == host.size() ? '.' : host[i];
        if (c == '.') {
            if (!digits || ++segments > 4) return false;
            result = (result << 8) | octet;
            octet = digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 3 || (octet = octet * 10 + c - '0') > 255) return false;
        } else return false;
    }
    return segments == 4;
}

bool valid_host(const std::string &host) {
    if (host.empty() || host.size() > 100 || host.front() == '.' || host.back() == '.') return false;
    size_t label = 0;
    bool numeric = true;
    for (size_t i = 0; i < host.size(); ++i) {
        const char c = host[i];
        if (c == '.') {
            if (!label || label > 63 || host[i - 1] == '-') return false;
            label = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            if (!label && c == '-') return false;
            ++label;
            if (c < '0' || c > '9') numeric = false;
        } else return false;
    }
    if (!label || label > 63 || host.back() == '-') return false;
    uint32_t parsed;
    return !numeric || ipv4(host, parsed);
}

ValidationError normalize_host(Submission &out, bool explicit_port) {
    auto &host = out.host;
    const size_t first = host.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return ValidationError::InvalidHost;
    host = host.substr(first, host.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
    });
    if (host.rfind("https://", 0) == 0) { host.erase(0, 8); out.https = true; if (!explicit_port) out.port = 443; }
    else if (host.rfind("http://", 0) == 0) { host.erase(0, 7); out.https = false; if (!explicit_port) out.port = 80; }
    if (!host.empty() && host.back() == '/') host.pop_back();
    if (host.find_first_of("/@?#[]") != std::string::npos) return ValidationError::InvalidHost;
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        if (!valid_port(host.substr(colon + 1), out.port)) return ValidationError::InvalidPort;
        host.resize(colon);
    }
    if (!valid_host(host)) return ValidationError::InvalidHost;
    if (!out.https) {
        uint32_t address;
        const bool local = ipv4(host, address) ? ((address >> 24) == 10 ||
            (address >> 20) == 0xac1 || (address >> 16) == 0xc0a8 ||
            (address >> 24) == 127 || (address >> 16) == 0xa9fe) :
            (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0);
        if (!local) return ValidationError::PublicHttp;
    }
    return ValidationError::None;
}
} // namespace

ValidationError parse_submission(const std::string &body, const std::string &nonce,
                                const InitialConfig &initial, Submission &out) {
    (void)initial; // Old passwords are deliberately never fetched or reused by HTTP.
    if (body.empty() || body.size() > kMaxBody || nul_escape(body) || !flat_json(body)) return ValidationError::InvalidJson;
    using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
    const char *end = nullptr;
    Json root(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, &end, true), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) return ValidationError::InvalidJson;
    static const std::set<std::string> fields = {"nonce", "mode", "wifi_ssid", "wifi_password",
        "receiver_ssid", "receiver_password", "host", "port", "https"};
    std::set<std::string> seen;
    for (const cJSON *item = root->child; item; item = item->next) {
        if (!item->string || !fields.count(item->string) || !seen.insert(item->string).second)
            return ValidationError::UnknownField;
    }
    std::string sent_nonce;
    if (!string_field(root.get(), "nonce", sent_nonce) || !same_nonce(nonce, sent_nonce))
        return ValidationError::InvalidNonce;
    Submission value;
    if (!string_field(root.get(), "mode", value.mode) ||
        (value.mode != "automatic" && value.mode != "manual" && value.mode != "receiver"))
        return ValidationError::InvalidMode;
    if (!string_field(root.get(), "wifi_ssid", value.wifi_ssid) ||
        !string_field(root.get(), "wifi_password", value.wifi_password) ||
        !safe_text(value.wifi_ssid, 32) || !safe_text(value.wifi_password, 63) ||
        (!value.wifi_password.empty() && value.wifi_password.size() < 8) ||
        (value.wifi_ssid.empty() && !value.wifi_password.empty())) return ValidationError::InvalidWifi;
    if (!string_field(root.get(), "receiver_ssid", value.receiver_ssid) ||
        !string_field(root.get(), "receiver_password", value.receiver_password) ||
        !safe_text(value.receiver_ssid, 32) || !safe_text(value.receiver_password, 63))
        return ValidationError::InvalidReceiver;
    if (!string_field(root.get(), "host", value.host) || value.host.size() > 128)
        return ValidationError::InvalidHost;
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(root.get(), "port");
    if (port) {
        if (!cJSON_IsNumber(port) || !std::isfinite(port->valuedouble) || port->valuedouble < 1 ||
            port->valuedouble > 65535 || std::floor(port->valuedouble) != port->valuedouble)
            return ValidationError::InvalidPort;
        value.port = static_cast<uint16_t>(port->valuedouble);
    }
    const cJSON *https = cJSON_GetObjectItemCaseSensitive(root.get(), "https");
    if (https && !cJSON_IsBool(https)) return ValidationError::InvalidHost;
    if (https) value.https = cJSON_IsTrue(https);
    if (value.mode == "receiver") {
        if (value.receiver_ssid.empty() || value.receiver_password.size() < 8)
            return ValidationError::InvalidReceiver;
        value.wifi_ssid.clear(); value.wifi_password.clear(); value.host.clear();
    } else {
        value.receiver_ssid.clear(); value.receiver_password.clear();
        if (value.mode == "manual") {
            const auto error = normalize_host(value, port != nullptr);
            if (error != ValidationError::None) return error;
        } else value.host.clear();
    }
    out = std::move(value);
    return ValidationError::None;
}

const char *error_message(ValidationError error, bool english) {
    switch (error) {
        case ValidationError::InvalidNonce: return english ? "This page has expired. Reopen phone setup on made." : "此页面已失效，请在码得重新打开手机配置。";
        case ValidationError::InvalidMode: return english ? "Choose a connection method." : "请选择连接方式。";
        case ValidationError::InvalidWifi: return english ? "Wi-Fi name: 1–32 bytes. Password: 8–63 bytes, or blank for an open network." : "Wi-Fi 名称为 1–32 字节；密码为 8–63 字节，开放网络可留空。";
        case ValidationError::InvalidReceiver: return english ? "Enter the receiver Wi-Fi name (1–32 bytes) and password (8–63 bytes) shown on the computer." : "请填写电脑端显示的接收端热点名称（1–32 字节）和密码（8–63 字节）。";
        case ValidationError::InvalidHost: return english ? "Enter a hostname, IPv4 address, or HTTP(S) URL without a path, account, or query." : "请输入主机名、IPv4 地址或 HTTP(S) 地址；不要包含路径、账号或查询参数。";
        case ValidationError::InvalidPort: return english ? "Port must be an integer from 1 to 65535." : "端口须为 1–65535 的整数。";
        case ValidationError::PublicHttp: return english ? "Public addresses require HTTPS." : "公网地址必须使用 HTTPS。";
        default: return english ? "Invalid request. Reload this page and try again." : "请求格式不正确，请刷新页面后重试。";
    }
}

bool json_content_type(const std::string &value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
    });
    const auto separator = lower.find(';');
    auto type = lower.substr(0, separator);
    while (!type.empty() && type.back() == ' ') type.pop_back();
    if (type != "application/json") return false;
    if (separator == std::string::npos) return true;
    auto charset = lower.substr(separator + 1);
    charset.erase(0, charset.find_first_not_of(' '));
    return charset == "charset=utf-8" || charset == "charset=\"utf-8\"";
}

std::string json_for_script(const std::string &value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char c : value) {
        if (c == '<') result += "\\u003c";
        else if (c == '>') result += "\\u003e";
        else if (c == '&') result += "\\u0026";
        else result += static_cast<char>(c);
    }
    return result;
}

std::string qr_escape(const std::string &value) {
    std::string result;
    for (const char c : value) {
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') result += '\\';
        result += c;
    }
    return result;
}

size_t dns_reply(const uint8_t *query, size_t length, uint8_t *out, size_t capacity) {
    if (!query || !out || length < 17 || length > 512 || (query[2] & 0xf8) ||
        query[4] != 0 || query[5] != 1) return 0;
    size_t end = 12, name_bytes = 0;
    for (;;) {
        if (end >= length) return 0;
        const unsigned label = query[end++];
        if (!label) break;
        if (label > 63 || end + label >= length || (name_bytes += label + 1) > 254) return 0;
        end += label;
    }
    if (end + 4 > length || query[end + 2] != 0 || query[end + 3] != 1) return 0;
    const bool address = query[end] == 0 && query[end + 1] == 1;
    end += 4;
    const size_t total = end + (address ? 16 : 0);
    if (total > capacity) return 0;
    std::memcpy(out, query, end);
    out[2] = 0x84 | (query[2] & 1); out[3] = 0;
    out[6] = 0; out[7] = address ? 1 : 0;
    out[8] = out[9] = out[10] = out[11] = 0;
    if (address) {
        const uint8_t answer[16] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 8, 1};
        std::memcpy(out + end, answer, sizeof(answer));
    }
    return total;
}
std::string render_page(bool use_english, const InitialConfig &initial, const std::string &session_nonce) {
    cJSON *config = cJSON_CreateObject();
    if (!config) return {};
    cJSON_AddBoolToObject(config, "english", use_english);
    cJSON_AddStringToObject(config, "nonce", session_nonce.c_str());
    cJSON_AddStringToObject(config, "mode", initial.mode.c_str());
    cJSON_AddStringToObject(config, "wifi_ssid", initial.wifi_ssid.c_str());
    cJSON_AddStringToObject(config, "receiver_ssid", initial.receiver_ssid.c_str());
    cJSON_AddStringToObject(config, "host", initial.host.c_str());
    cJSON_AddNumberToObject(config, "port", initial.port ? initial.port : 443);
    cJSON_AddBoolToObject(config, "https", initial.https);
    char *serialized = cJSON_PrintUnformatted(config);
    cJSON_Delete(config);
    if (!serialized) return {};
    const auto escaped = json_for_script(serialized);
    cJSON_free(serialized);
    std::string result = kPage;
    constexpr char placeholder[] = "__MADE_SETUP_CONFIG__";
    result.replace(result.find(placeholder), sizeof(placeholder) - 1, escaped);
    return result;
}

} // namespace vibe_phone_setup::detail
