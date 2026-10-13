// SPDX-License-Identifier: Apache-2.0
#include "made_sprite_cache.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>

#include <cJSON.h>
#include <esp_log.h>
#include <mbedtls/sha256.h>

#include <vector>

namespace made_sprite_cache {
namespace {

constexpr const char *kTag = "made_sprite";
constexpr const char *kMount = "/spiffs";
constexpr size_t kChunkBytes = 192 * 1024;   // 单请求上限，兼容 USB 帧转发 256KB
constexpr size_t kResponseLimit = 200 * 1024;

// SPIFFS 的对象名长度上限是 SPIFFS_OBJ_NAME_LEN-1 = 31 字节，且算的是
// VFS 交给 SPIFFS_open 的整串（含前导 '/'）。缓存名 = '/' + 前缀 + 文件名 + 临时后缀，
// 而合同允许的文件名最长 20 字节（docs/SPRITES.md），所以前缀必须 ≤ 31-1-20-1 = 9。
// 这里取 8：'s' + 6 位哈希 + '_'。哈希只用于命名，不承担安全语义。
constexpr const char *kTempSuffix = "~";
constexpr size_t kObjectNameLimit = 31;

std::string prefixFor(const std::string &provider)
{
    // FNV-1a 32bit → 6 位十六进制。
    uint32_t hash = 2166136261u;
    for (const char c : provider) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    char text[12];
    snprintf(text, sizeof(text), "s%06x_", static_cast<unsigned>(hash) & 0xffffffu);
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
    // VFS 交给 SPIFFS_open 的是「去掉挂载点、保留前导 '/'」的路径，长度限制算的是它。
    const std::string relative = path.substr(std::strlen(kMount));
    const std::string temporary = path + kTempSuffix;
    // 名字超限是 SPIFFS 最沉默的一类失败：open 直接拿不到句柄。提前判掉并留痕。
    if (relative.size() + 1 > kObjectNameLimit) {
        ESP_LOGE(kTag, "[sprite] write_name_too_long object_len=%u limit=%u name='%s'",
            static_cast<unsigned>(relative.size() + 1), static_cast<unsigned>(kObjectNameLimit),
            relative.c_str());
        return false;
    }
    errno = 0;
    FILE *file = fopen(temporary.c_str(), "wb");
    if (!file) {
        ESP_LOGE(kTag, "[sprite] write_open_failed errno=%d(%s) name='%s'",
            errno, std::strerror(errno), temporary.c_str());
        return false;
    }
    errno = 0;
    const size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    if (written != data.size()) {
        ESP_LOGE(kTag, "[sprite] write_short wrote=%u want=%u errno=%d(%s)",
            static_cast<unsigned>(written), static_cast<unsigned>(data.size()),
            errno, std::strerror(errno));
        unlink(temporary.c_str());
        return false;
    }
    errno = 0;
    // SPIFFS 的 rename 不覆盖已存在的目标（否则 EIO），先摘掉旧文件再改名。
    unlink(path.c_str());
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        ESP_LOGE(kTag, "[sprite] write_rename_failed errno=%d(%s) from='%s' to='%s'",
            errno, std::strerror(errno), temporary.c_str(), path.c_str());
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
                ESP_LOGE(kTag, "[sprite] fetch_failed file=%s offset=%u status=%d",
                    file.file.c_str(), static_cast<unsigned>(data.size()), status);
                return false;
            }
            if (chunk.empty()) break; // 服务端明确返回空：offset 已到文件尾
            data += chunk;
        }
        if (data.size() != file.bytes || sha256Hex(data) != file.sha256) {
            ESP_LOGE(kTag, "[sprite] verify_failed file=%s want=%u/%s got=%u/%s",
                file.file.c_str(), static_cast<unsigned>(file.bytes), file.sha256.c_str(),
                static_cast<unsigned>(data.size()), sha256Hex(data).c_str());
            return false;
        }
        if (!writeAtomic(target, data)) {
            ESP_LOGE(kTag, "[sprite] write_failed file=%s bytes=%u path=%s",
                file.file.c_str(), static_cast<unsigned>(data.size()), target.c_str());
            return false;
        }
        ESP_LOGI(kTag, "[sprite] stored file=%s bytes=%u", file.file.c_str(),
            static_cast<unsigned>(data.size()));
        downloaded.push_back(prefix + file.file);
    }

    // manifest.json 最后拉、最后写：它一落盘就代表整套素材可切换。
    std::string manifest;
    int status = 0;
    if (!fetch(fileAsPath(provider, "manifest.json"), manifest, status) || status != 200 ||
        manifest.empty() || manifest.size() > 20 * 1024) {
        ESP_LOGE(kTag, "[sprite] manifest_fetch_failed status=%d bytes=%u", status,
            static_cast<unsigned>(manifest.size()));
        return false;
    }
    bool version_ok = false;
    if (cJSON *root = cJSON_Parse(manifest.c_str())) {
        const auto *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        version_ok = cJSON_IsNumber(version) &&
            static_cast<uint32_t>(version->valueint) == sprite.version;
        cJSON_Delete(root);
    }
    if (!version_ok || !writeAtomic(manifest_path, manifest)) {
        ESP_LOGE(kTag, "[sprite] manifest_rejected version_ok=%d bytes=%u",
            version_ok ? 1 : 0, static_cast<unsigned>(manifest.size()));
        return false;
    }
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
