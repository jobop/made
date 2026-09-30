// SPDX-License-Identifier: Apache-2.0
#include "vibe_i18n.hpp"
#include "dictionary.hpp"
#include <atomic>
#include <cstring>
#include <mutex>
#include "nvs.h"

namespace vibe_i18n {
namespace {
std::atomic<Locale> current{Locale::Chinese};
std::atomic<uint32_t> version{0};
std::atomic<bool> follow{true};
Locale bridge_locale = Locale::Chinese;
bool has_bridge_locale = false;
std::mutex mutex;
bool initialized = false;
constexpr char nvs_namespace[] = "made_i18n";
const Entry *find(const char *value) {
    if (!value) return nullptr;
    for (const auto &entry : dictionary)
        if ((entry.id && !std::strcmp(value, entry.id)) || !std::strcmp(value, entry.zh) ||
            !std::strcmp(value, entry.en)) return &entry;
    return nullptr;
}
}
bool parse_locale(const char *value, Locale &result) {
    if (!value) return false;
    if (!std::strcmp(value, "zh-CN")) { result = Locale::Chinese; return true; }
    if (!std::strcmp(value, "en")) { result = Locale::English; return true; }
    return false;
}
void initialize() {
    std::lock_guard<std::mutex> lock(mutex);
    if (initialized) return;
    nvs_handle_t handle;
    if (nvs_open(nvs_namespace, NVS_READONLY, &handle) == ESP_OK) {
        char stored[8]{}; size_t length = sizeof(stored); Locale parsed;
        if (nvs_get_str(handle, "locale", stored, &length) == ESP_OK && parse_locale(stored, parsed))
            current.store(parsed);
        length = sizeof(stored);
        if (nvs_get_str(handle, "follow_bridge", stored, &length) == ESP_OK && !std::strcmp(stored, "0"))
            follow.store(false);
        length = sizeof(stored);
        if (nvs_get_str(handle, "bridge_locale", stored, &length) == ESP_OK && parse_locale(stored, parsed)) {
            bridge_locale = parsed;
            has_bridge_locale = true;
        }
        nvs_close(handle);
    }
    initialized = true;
}
namespace {
const char *name(Locale value) { return value == Locale::English ? "en" : "zh-CN"; }
// Called with mutex held. Commit mode, effective language and remembered bridge
// language together; publish only after a successful commit.
bool save(Locale next, bool next_follow, bool remember, Locale remembered) {
    const bool changed = current.load() != next || follow.load() != next_follow;
    const bool bridge_changed = remember && (!has_bridge_locale || bridge_locale != remembered);
    if (!changed && !bridge_changed) return true;
    nvs_handle_t handle;
    if (nvs_open(nvs_namespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    auto result = nvs_set_str(handle, "locale", name(next));
    if (result == ESP_OK) result = nvs_set_str(handle, "follow_bridge", next_follow ? "1" : "0");
    if (result == ESP_OK && remember) result = nvs_set_str(handle, "bridge_locale", name(remembered));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return false;
    current.store(next);
    follow.store(next_follow);
    if (remember) { bridge_locale = remembered; has_bridge_locale = true; }
    if (changed) version.fetch_add(1);
    return true;
}
}
bool set_locale(const char *value) {
    Locale parsed;
    if (!parse_locale(value, parsed)) return false;
    initialize();
    std::lock_guard<std::mutex> lock(mutex);
    return save(parsed, false, false, bridge_locale);
}
bool set_follow_bridge() {
    initialize();
    std::lock_guard<std::mutex> lock(mutex);
    return save(has_bridge_locale ? bridge_locale : current.load(), true, false, bridge_locale);
}
bool follows_bridge() { return follow.load(); }
bool apply_bridge_locale(const char *value) {
    Locale parsed;
    if (!parse_locale(value, parsed)) return false;
    initialize();
    std::lock_guard<std::mutex> lock(mutex);
    return save(follow.load() ? parsed : current.load(), follow.load(), true, parsed);
}
std::string request_path(const std::string &path) {
    // Query is metadata only; do not touch pairing, transport frames or user text.
    std::lock_guard<std::mutex> lock(mutex);
    if (follow.load() || path.rfind("/device/", 0) != 0) return path;
    return path + (path.find('?') == std::string::npos ? "?" : "&") + "uiLocale=" + name(current.load());
}
Locale locale() { return current.load(); }
const char *locale_name() { return locale() == Locale::English ? "en" : "zh-CN"; }
uint32_t revision() { return version.load(); }
bool known(const char *key) { return find(key); }
const char *tr(const char *key) {
    const auto *entry = find(key);
    return entry ? (locale() == Locale::English ? entry->en : entry->zh) : key;
}
std::string transport_error(const std::string &value) {
    static constexpr const char *keys[] = {
        "电脑接收端响应超时", "电脑返回无效响应帧", "接收端未提供此接口", "请求路径或长度无效",
        "请求体超过接收端限制", "请求头过长", "接收端正在处理另一请求", "电脑未连接或 USB 转发失败"
    };
    for (const auto *key : keys) if (value == key) return tr(key);
    return value;
}
std::string message(const std::string &value) {
    if (const auto *entry = find(value.c_str())) return locale() == Locale::English ? entry->en : entry->zh;
    // Status-code suffixes are data. Translate only our known prefix.
    for (const auto &entry : dictionary) {
        const size_t en_size = std::strlen(entry.en), zh_size = std::strlen(entry.zh);
        if (en_size < 6 || std::string(entry.en).find("HTTP ") == std::string::npos) continue;
        if (value.compare(0, zh_size, entry.zh) == 0)
            return std::string(locale() == Locale::English ? entry.en : entry.zh) + value.substr(zh_size);
        if (value.compare(0, en_size, entry.en) == 0)
            return std::string(locale() == Locale::English ? entry.en : entry.zh) + value.substr(en_size);
    }
    return value;
}
}
