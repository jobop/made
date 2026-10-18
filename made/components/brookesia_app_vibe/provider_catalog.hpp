// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "cJSON.h"
#include "provider_icon.hpp"

// This model has no UI or transport dependencies. VibeCoding guards every
// catalog access with model_mutex_; requests carry stable IDs across refreshes.
namespace vibe_provider {
constexpr size_t MAX_PROVIDERS = 12;
constexpr size_t MAX_LABEL_BYTES = 64;
// 形象素材清单（/device/config 里 provider.sprite 的快照，见 docs/SPRITES.md）。
// 只带版本与文件清单；分片拉取与布局 manifest 由固件另行下载。
struct SpriteFile {
    std::string file;
    uint32_t bytes = 0;
    std::string sha256;
};
struct SpriteInfo {
    uint32_t version = 0;
    std::vector<SpriteFile> files;
    bool valid() const { return version > 0 && !files.empty(); }
};
struct Provider {
    std::string id;
    std::string label;
    bool available = false;
    std::string reason;
    Icon icon;
    SpriteInfo sprite;
    bool external_unscoped = true;
    bool model = false;
    bool cancel = false;
    bool progress = false;
    std::string selected_session_id;
};

inline std::string text(const cJSON *item, const char *key)
{
    const auto *value = cJSON_GetObjectItemCaseSensitive(item, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

inline SpriteInfo parseSprite(const cJSON *sprite)
{
    SpriteInfo info;
    if (!cJSON_IsObject(sprite)) return info;
    const auto *version = cJSON_GetObjectItemCaseSensitive(sprite, "version");
    if (!cJSON_IsNumber(version) || version->valueint <= 0) return info;
    const auto *files = cJSON_GetObjectItemCaseSensitive(sprite, "files");
    if (!cJSON_IsArray(files)) return info;
    info.version = static_cast<uint32_t>(version->valueint);
    const cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, files) {
        if (!cJSON_IsObject(entry) || info.files.size() >= 16) break;
        const auto file = text(entry, "file");
        const auto *bytes = cJSON_GetObjectItemCaseSensitive(entry, "bytes");
        const auto sha = text(entry, "sha256");
        // 文件名在 SPIFFS 平铺缓存里拼在 "s<hash6>_" 之后、临时后缀 "~" 之前，
        // 而 SPIFFS 对象名（含前导 '/'）只有 31 字节预算，必须收得很紧。
        // 扩展名含 .gif：GIF 动作素材（actions[].gif）也在 files 清单里，
        // 只收 .png 的话设备侧直接丢条目，动作 GIF 永远下载不到。
        const bool ext_ok = file.size() >= 5 && file.size() <= 20 &&
            (file.rfind(".png") == file.size() - 4 || file.rfind(".gif") == file.size() - 4);
        if (!ext_ok ||
            file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos ||
            sha.size() != 64 || !cJSON_IsNumber(bytes) || bytes->valueint <= 0) continue;
        info.files.push_back({file, static_cast<uint32_t>(bytes->valueint), sha});
    }
    if (info.files.empty()) info.version = 0;
    return info;
}

inline bool validId(const std::string &id)
{
    return !id.empty() && id.size() <= 64 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
}

inline std::string boundedText(std::string value, size_t limit)
{
    if (value.size() > limit) {
        size_t end = limit;
        while (end && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
        value.resize(end);
    }
    // Metadata is a one-line label/reason, never LVGL control text.
    for (auto &c : value) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return value;
}

inline bool parse(const cJSON *items, std::vector<Provider> &result)
{
    if (!cJSON_IsArray(items)) return false;
    result.clear();
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
        if (result.size() >= MAX_PROVIDERS) break;
        const auto id = text(item, "id");
        if (!cJSON_IsObject(item) || !validId(id) ||
            std::any_of(result.begin(), result.end(), [&id](const Provider &p) { return p.id == id; })) continue;
        Provider provider;
        provider.id = id;
        const auto label = boundedText(text(item, "label"), MAX_LABEL_BYTES);
        if (!label.empty()) provider.label = label;
        if (provider.label.empty()) provider.label = id;
        provider.available = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "available"));
        provider.reason = boundedText(text(item, "reason"), 240);
        provider.icon = parseIcon(cJSON_GetObjectItemCaseSensitive(item, "icon"));
        provider.sprite = parseSprite(cJSON_GetObjectItemCaseSensitive(item, "sprite"));
        const auto *capabilities = cJSON_GetObjectItemCaseSensitive(item, "capabilities");
        if (cJSON_IsObject(capabilities)) {
            // Unknown/missing session guarantees are treated conservatively.
            provider.external_unscoped = text(capabilities, "session") != "native";
            provider.model = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "model"));
            provider.cancel = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "cancel"));
            provider.progress = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(capabilities, "progress"));
        }
        result.push_back(std::move(provider));
    }
    return true;
}

inline size_t replace(std::vector<Provider> &catalog, std::vector<Provider> next,
                      const std::string &selected_id)
{
    size_t selected = 0;
    for (size_t index = 0; index < next.size(); ++index) {
        const auto previous = std::find_if(catalog.begin(), catalog.end(),
            [&next, index](const Provider &p) { return p.id == next[index].id; });
        if (previous != catalog.end()) next[index].selected_session_id = previous->selected_session_id;
        if (next[index].id == selected_id) selected = index;
    }
    catalog = std::move(next);
    return selected;
}
} // namespace vibe_provider
