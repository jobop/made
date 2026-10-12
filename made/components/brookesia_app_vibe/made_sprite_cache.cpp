// SPDX-License-Identifier: Apache-2.0
#include "made_sprite_cache.hpp"

#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>

#include <cJSON.h>
#include <mbedtls/sha256.h>

#include <vector>

namespace made_sprite_cache {
namespace {

constexpr const char *kMount = "/spiffs";
constexpr size_t kChunkBytes = 192 * 1024;   // 单请求上限，兼容 USB 帧转发 256KB
constexpr size_t kResponseLimit = 200 * 1024;

std::string prefixFor(const std::string &provider)
{
    // FNV-1a 32bit → 8 位十六进制。只用于命名，不承担安全语义。
    uint32_t hash = 2166136261u;
    for (const char c : provider) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    char text[14];
    snprintf(text, sizeof(text), "sp_%08x_", static_cast<unsigned>(hash));
    return text;
}

std::string pathFor(const std::string &prefix, const std::string &name)
{
    return std::string(kMount) + "/" + prefix + name;
}

std::string sha256Hex(const std::string &data)
{
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest, 0);
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 64);
}

bool writeAtomic(const std::string &path, const std::string &data)
{
    const std::string temporary = path + ".tmp";
    FILE *file = fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), file) == data.size();
    fclose(file);
    if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

bool readIfPresent(const std::string &path, std::string &out)
{
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) return false;
    out.clear();
    char buffer[4096];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, read);
    fclose(file);
    return true;
}

std::string fileAsPath(const std::string &provider, const std::string &name)
{
    return "/device/sprite/" + provider + "/" + name;
}

// 目录里以该前缀命名的文件清单（不递归；SPIFFS 本来就是平铺的）。
std::vector<std::string> listPrefixed(const std::string &prefix)
{
    std::vector<std::string> names;
    DIR *dir = opendir(kMount);
    if (!dir) return names;
    while (const dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind(prefix, 0) == 0) names.push_back(name);
    }
    closedir(dir);
    return names;
}

// 版本变更时清掉旧版本的文件，防止 4MB 的 assets 分区被历史版本吃满。
void purgeExcept(const std::string &prefix, const std::vector<std::string> &keep)
{
    for (const auto &name : listPrefixed(prefix)) {
        if (std::find(keep.begin(), keep.end(), name) != keep.end()) continue;
        unlink(pathFor(prefix, name).c_str());
    }
}

} // namespace

std::string baseDir(const std::string &provider)
{
    return std::string(kMount) + "/" + prefixFor(provider);
}

bool ensure(const std::string &provider, const vibe_provider::SpriteInfo &sprite,
            const Fetch &fetch)
{
    if (provider.empty() || !sprite.valid() || !fetch) return false;
    const std::string prefix = prefixFor(provider);
    const std::string manifest_path = pathFor(prefix, "manifest.json");

    // 快路径：manifest 已提交且版本一致，逐个校验哈希即可，不动网络。
    std::string cached;
    if (readIfPresent(manifest_path, cached)) {
        bool same_version = false;
        if (cJSON *root = cJSON_Parse(cached.c_str())) {
            const auto *version = cJSON_GetObjectItemCaseSensitive(root, "version");
            same_version = cJSON_IsNumber(version) &&
                static_cast<uint32_t>(version->valueint) == sprite.version;
            cJSON_Delete(root);
        }
        if (same_version) {
            bool intact = true;
            for (const auto &file : sprite.files) {
                std::string data;
                if (!readIfPresent(pathFor(prefix, file.file), data) ||
                    data.size() != file.bytes || sha256Hex(data) != file.sha256) {
                    intact = false;
                    break;
                }
            }
            if (intact) return true;
        }
    }

    // 慢路径：逐文件分片下载 → 哈希校验 → 原子落盘。
    std::vector<std::string> downloaded;
    for (const auto &file : sprite.files) {
        const std::string target = pathFor(prefix, file.file);
        std::string existing;
        if (readIfPresent(target, existing) && existing.size() == file.bytes &&
            sha256Hex(existing) == file.sha256) {
            downloaded.push_back(prefix + file.file);
            continue;
        }
        std::string data;
        int status = 0;
        while (data.size() < file.bytes) {
            std::string chunk;
            char query[64];
            snprintf(query, sizeof(query), "?offset=%u&len=%u",
                     static_cast<unsigned>(data.size()), static_cast<unsigned>(kChunkBytes));
            if (!fetch(fileAsPath(provider, file.file) + query, chunk, status) || status != 200) {
                return false;
            }
            if (chunk.empty()) break; // 服务端明确返回空：offset 已到文件尾
            data += chunk;
        }
        if (data.size() != file.bytes || sha256Hex(data) != file.sha256) return false;
        if (!writeAtomic(target, data)) return false;
        downloaded.push_back(prefix + file.file);
    }

    // manifest.json 最后拉、最后写：它一落盘就代表整套素材可切换。
    std::string manifest;
    int status = 0;
    if (!fetch(fileAsPath(provider, "manifest.json"), manifest, status) || status != 200 ||
        manifest.empty() || manifest.size() > 20 * 1024) {
        return false;
    }
    bool version_ok = false;
    if (cJSON *root = cJSON_Parse(manifest.c_str())) {
        const auto *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        version_ok = cJSON_IsNumber(version) &&
            static_cast<uint32_t>(version->valueint) == sprite.version;
        cJSON_Delete(root);
    }
    if (!version_ok || !writeAtomic(manifest_path, manifest)) return false;
    downloaded.push_back(prefix + "manifest.json");
    purgeExcept(prefix, downloaded);
    return true;
}

bool loadManifest(const std::string &provider, std::string &json)
{
    if (provider.empty()) return false;
    return readIfPresent(pathFor(prefixFor(provider), "manifest.json"), json) && !json.empty();
}

} // namespace made_sprite_cache
