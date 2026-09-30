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
struct Provider {
    std::string id;
    std::string label;
    bool available = false;
    std::string reason;
    Icon icon;
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
