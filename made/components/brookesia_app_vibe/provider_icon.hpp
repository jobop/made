// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "cJSON.h"

// The bridge owns artwork. Firmware only decodes a small, bounded pixel format;
// no names, image URLs, drawing templates, or image-file parsers live here.
namespace vibe_provider {
constexpr size_t MAX_ICON_SIDE = 48;
constexpr size_t MAX_ICON_PIXELS = MAX_ICON_SIDE * MAX_ICON_SIDE;
struct Icon {
    uint8_t width = 0;
    uint8_t height = 0;
    uint8_t colors = 0;
    std::array<uint32_t, 16> rgba{};
    std::vector<uint8_t> packed;
    uint32_t background = 0x263B59;
    uint32_t accent = 0x8294AC;
    bool valid() const { return width && height && colors && !packed.empty(); }
    bool operator==(const Icon &other) const {
        return width == other.width && height == other.height && colors == other.colors &&
            rgba == other.rgba && packed == other.packed &&
            background == other.background && accent == other.accent;
    }
};

inline bool hexColor(const char *value, size_t digits, uint32_t &result)
{
    if (!value || std::char_traits<char>::length(value) != digits) return false;
    uint32_t color = 0;
    for (size_t i = 0; i < digits; ++i) {
        const char c = value[i];
        const int n = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (n < 0) return false;
        color = (color << 4) | static_cast<unsigned>(n);
    }
    result = color;
    return true;
}

inline int base64Digit(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

inline bool unpackBase64(const std::string &encoded, size_t expected, std::vector<uint8_t> &bytes)
{
    if (!expected || expected > (MAX_ICON_PIXELS + 1) / 2 ||
        encoded.size() != ((expected + 2) / 3) * 4) return false;
    bytes.clear();
    bytes.reserve(expected);
    for (size_t i = 0; i < encoded.size(); i += 4) {
        const int a = base64Digit(encoded[i]), b = base64Digit(encoded[i + 1]);
        const int c = encoded[i + 2] == '=' ? 0 : base64Digit(encoded[i + 2]);
        const int d = encoded[i + 3] == '=' ? 0 : base64Digit(encoded[i + 3]);
        const size_t remain = expected - bytes.size();
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (encoded[i + 2] == '=') != (remain == 1) ||
            (encoded[i + 3] == '=') != (remain <= 2) ||
            (remain == 1 && (b & 15)) || (remain == 2 && (c & 3))) return false;
        bytes.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (remain > 1) bytes.push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
        if (remain > 2) bytes.push_back(static_cast<uint8_t>((c << 6) | d));
    }
    return bytes.size() == expected;
}

inline Icon parseIcon(const cJSON *value)
{
    Icon icon;
    if (!cJSON_IsObject(value)) return icon;
    const auto *format = cJSON_GetObjectItemCaseSensitive(value, "format");
    const auto *width = cJSON_GetObjectItemCaseSensitive(value, "width");
    const auto *height = cJSON_GetObjectItemCaseSensitive(value, "height");
    const auto *palette = cJSON_GetObjectItemCaseSensitive(value, "palette");
    const auto *data = cJSON_GetObjectItemCaseSensitive(value, "data");
    if (!cJSON_IsString(format) || std::string(format->valuestring) != "indexed4" ||
        !cJSON_IsNumber(width) || !cJSON_IsNumber(height) ||
        width->valuedouble != width->valueint || height->valuedouble != height->valueint ||
        width->valueint < 1 || width->valueint > static_cast<int>(MAX_ICON_SIDE) ||
        height->valueint < 1 || height->valueint > static_cast<int>(MAX_ICON_SIDE) ||
        !cJSON_IsArray(palette) || cJSON_GetArraySize(palette) < 1 || cJSON_GetArraySize(palette) > 16 ||
        !cJSON_IsString(data) || !data->valuestring) return {};
    icon.width = static_cast<uint8_t>(width->valueint);
    icon.height = static_cast<uint8_t>(height->valueint);
    icon.colors = static_cast<uint8_t>(cJSON_GetArraySize(palette));
    for (unsigned i = 0; i < icon.colors; ++i) {
        const auto *color = cJSON_GetArrayItem(palette, i);
        if (!cJSON_IsString(color) || !hexColor(color->valuestring, 8, icon.rgba[i])) return {};
    }
    for (const auto *key : {"background", "accent"}) {
        const auto *color = cJSON_GetObjectItemCaseSensitive(value, key);
        uint32_t parsed = 0;
        if (color && (!cJSON_IsString(color) || !color->valuestring || color->valuestring[0] != '#' ||
            !hexColor(color->valuestring + 1, 6, parsed))) return {};
        if (color) (std::string(key) == "background" ? icon.background : icon.accent) = parsed;
    }
    const size_t count = static_cast<size_t>(icon.width) * icon.height;
    if (!unpackBase64(data->valuestring, (count + 1) / 2, icon.packed)) return {};
    for (size_t i = 0; i < count; ++i) {
        const auto index = (i & 1) ? icon.packed[i / 2] & 15 : icon.packed[i / 2] >> 4;
        if (index >= icon.colors) return {};
    }
    if ((count & 1) && (icon.packed.back() & 15)) return {};
    return icon;
}

// LVGL ARGB8888 uses BGRA bytes in memory. Keep alpha straight (not premultiplied).
inline bool decodeIconBgra(const Icon &icon, uint8_t *out, size_t capacity)
{
    const size_t count = static_cast<size_t>(icon.width) * icon.height;
    if (!icon.valid() || !out || count > MAX_ICON_PIXELS || icon.colors > 16 ||
        icon.packed.size() != (count + 1) / 2 || capacity < count * 4) return false;
    for (size_t i = 0; i < count; ++i) {
        const auto index = (i & 1) ? icon.packed[i / 2] & 15 : icon.packed[i / 2] >> 4;
        if (index >= icon.colors) return false;
        const uint32_t rgba = icon.rgba[index];
        out[i * 4] = static_cast<uint8_t>(rgba >> 8);
        out[i * 4 + 1] = static_cast<uint8_t>(rgba >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(rgba >> 24);
        out[i * 4 + 3] = static_cast<uint8_t>(rgba);
    }
    return true;
}
// Normalize to a constant canvas before LVGL scaling. Its software transform
// has edge cases for one-pixel source dimensions; a transparent padded canvas
// also keeps descriptor geometry stable when plugins replace their artwork.
inline bool decodeIconCanvasBgra(const Icon &icon, uint8_t *out, size_t capacity)
{
    const size_t count = static_cast<size_t>(icon.width) * icon.height;
    if (!icon.valid() || !out || icon.width > MAX_ICON_SIDE || icon.height > MAX_ICON_SIDE ||
        icon.colors > 16 || icon.packed.size() != (count + 1) / 2 || capacity < MAX_ICON_PIXELS * 4) return false;
    std::fill(out, out + MAX_ICON_PIXELS * 4, 0);
    const unsigned side = std::max(icon.width, icon.height);
    const unsigned width = std::max(1u, static_cast<unsigned>(MAX_ICON_SIDE) * icon.width / side);
    const unsigned height = std::max(1u, static_cast<unsigned>(MAX_ICON_SIDE) * icon.height / side);
    const unsigned left = (MAX_ICON_SIDE - width) / 2, top = (MAX_ICON_SIDE - height) / 2;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const size_t pixel = (y * icon.height / height) * icon.width + x * icon.width / width;
            const unsigned index = (pixel & 1) ? icon.packed[pixel / 2] & 15 : icon.packed[pixel / 2] >> 4;
            if (index >= icon.colors) return false;
            const uint32_t rgba = icon.rgba[index];
            const size_t offset = ((top + y) * MAX_ICON_SIDE + left + x) * 4;
            out[offset] = static_cast<uint8_t>(rgba >> 8);
            out[offset + 1] = static_cast<uint8_t>(rgba >> 16);
            out[offset + 2] = static_cast<uint8_t>(rgba >> 24);
            out[offset + 3] = static_cast<uint8_t>(rgba);
        }
    }
    return true;
}
} // namespace vibe_provider
