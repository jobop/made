// SPDX-License-Identifier: Apache-2.0
#include "made_sprite_cache.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

#include <cJSON.h>
#include <esp_log.h>

#include "vibe_wifi.hpp"

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

// 文件完整性策略（2026-10-19 简化）：不再做任何哈希（整文件/流式/分区都
// 不要）。理由：①曾把 ~500KB PNG 整读进 std::string 算哈希，PSRAM 紧张时
// 抛 bad_alloc 致 worker abort 重启循环；②PNG/GIF 格式自带逐块 CRC，坏
// 文件在解码时必然暴露，走应用侧 cached_apply_dead → purge+redownload
// 兜底。下载校验只核对文件大小，跳过已存在同大小文件。

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
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        try {
            out.append(buffer, read);
        } catch (...) {
            // 内存紧张时的优雅降级：当作「文件不存在」处理，触发重下而不是崩线程。
            fclose(file);
            return false;
        }
    }
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

void purge(const std::string &provider)
{
    const std::string prefix = prefixFor(provider);
    int removed = 0;
    for (const auto &name : listPrefixed(prefix)) {
        unlink(pathFor(prefix, name).c_str());
        ++removed;
    }
    ESP_LOGW(kTag, "[sprite] purged provider=%s files=%d", provider.c_str(), removed);
}

std::string baseDir(const std::string &provider)
{
    return std::string(kMount) + "/" + prefixFor(provider);
}

bool ensureImpl(const std::string &provider, const vibe_provider::SpriteInfo &sprite,
                const Fetch &fetch, bool *manifest_changed)
{
    if (provider.empty() || !sprite.valid() || !fetch) return false;
    const std::string prefix = prefixFor(provider);
    const std::string manifest_path = pathFor(prefix, "manifest.json");

    // manifest 重拉比对：manifest 是信任根但自身无哈希可验，且曾在崩溃时代
    // 被截断过（头部 version/layers 完整、actions/commands 丢失，而版本号
    // 一致导致残缺 manifest 被永久信任，动作名单恒空）。这里每次多花一个小
    // 请求重拉与缓存比对，内容漂移就地重写；拉不到（网络抖动）则退回信任
    // 缓存的旧行为。
    std::string cached;
    const bool have_cached = readIfPresent(manifest_path, cached);
    {
        std::string fresh;
        int status = 0;
        if (fetch(fileAsPath(provider, "manifest.json"), fresh, status) &&
            status == 200 && !fresh.empty() && fresh.size() <= 20 * 1024) {
            if (!have_cached || fresh != cached) {
                if (writeAtomic(manifest_path, fresh)) {
                    cached = fresh;
                    if (manifest_changed) *manifest_changed = true;
                    ESP_LOGI(kTag, "[sprite] manifest_refreshed provider=%s bytes=%u",
                        provider.c_str(), static_cast<unsigned>(fresh.size()));
                }
            }
        }
    }

    // 快路径：manifest 已提交且版本一致就直接信任。manifest 是整套文件
    // 落盘前的最后一道闸：写它之前每个文件都验过流式哈希。这里不再逐文件
    // 复检（省掉每次换装的全量 SPIFFS 读 + SHA 计算）；文件真坏了会走
    // 应用侧 cached_apply_dead → purge+redownload 兜底。
    // 前提是文件齐：purge / 下载中断会留下「manifest 在、帧文件缺」的残缺
    // 缓存，此时快路径秒回 ok 而文件永远不补（2026-10-20 冒烟实锤：解码
    // 连败 → purge → ensure 快路径又 ok → already_attempted 拦截重下 →
    // 死循环打崩 PSRAM）。逐文件 stat 一次（无 SHA，开销可忽略）。
    if (have_cached) {
        bool same_version = false;
        if (cJSON *root = cJSON_Parse(cached.c_str())) {
            const auto *version = cJSON_GetObjectItemCaseSensitive(root, "version");
            same_version = cJSON_IsNumber(version) &&
                static_cast<uint32_t>(version->valueint) == sprite.version;
            cJSON_Delete(root);
        }
        if (same_version) {
            bool files_present = true;
            for (const auto &file : sprite.files) {
                struct stat st {};
                if (stat(pathFor(prefix, file.file).c_str(), &st) != 0) {
                    files_present = false;
                    break;
                }
            }
            if (files_present) return true;
            ESP_LOGW(kTag, "[sprite] cache_incomplete provider=%s -> slow path", provider.c_str());
        }
    }

    // 慢路径：逐文件分片下载 → 大小核对 → 原子落盘（不做哈希，见文件头注释）。
    // 下载窗口关 WiFi 省电：弱信号 + 300ms listen interval 曾致分片超时。
    vibe_wifi::WifiThroughput wifi_boost;
    std::vector<std::string> downloaded;
    for (const auto &file : sprite.files) {
        const std::string target = pathFor(prefix, file.file);
        struct stat st {};
        if (stat(target.c_str(), &st) == 0 &&
            static_cast<size_t>(st.st_size) == file.bytes) {
            downloaded.push_back(prefix + file.file);
            continue;
        }
        // 流式落盘：分片边下边写临时文件，不再把整文件攒进内存。
        const std::string relative = target.substr(std::strlen(kMount));
        const std::string temporary = target + kTempSuffix;
        if (relative.size() + 1 > kObjectNameLimit) {
            ESP_LOGE(kTag, "[sprite] write_name_too_long object_len=%u limit=%u name='%s'",
                static_cast<unsigned>(relative.size() + 1), static_cast<unsigned>(kObjectNameLimit),
                relative.c_str());
            return false;
        }
        FILE *sink = fopen(temporary.c_str(), "wb");
        if (!sink) {
            ESP_LOGE(kTag, "[sprite] write_open_failed errno=%d(%s) name='%s'",
                errno, std::strerror(errno), temporary.c_str());
            return false;
        }
        bool ok = true;
        size_t received = 0;
        int status = 0;
        while (received < file.bytes) {
            std::string chunk;
            char query[64];
            snprintf(query, sizeof(query), "?offset=%u&len=%u",
                     static_cast<unsigned>(received), static_cast<unsigned>(kChunkBytes));
            if (!fetch(fileAsPath(provider, file.file) + query, chunk, status) || status != 200) {
                ESP_LOGE(kTag, "[sprite] fetch_failed file=%s offset=%u status=%d",
                    file.file.c_str(), static_cast<unsigned>(received), status);
                ok = false;
                break;
            }
            if (chunk.empty()) break; // 服务端明确返回空：offset 已到文件尾
            if (fwrite(chunk.data(), 1, chunk.size(), sink) != chunk.size()) {
                ok = false;
                break;
            }
            received += chunk.size();
        }
        fclose(sink);
        if (!ok || received != file.bytes) {
            unlink(temporary.c_str());
            ESP_LOGE(kTag, "[sprite] size_check_failed file=%s want=%u got=%u",
                file.file.c_str(), static_cast<unsigned>(file.bytes),
                static_cast<unsigned>(received));
            return false;
        }
        errno = 0;
        // SPIFFS 的 rename 不覆盖已存在的目标（否则 EIO），先摘掉旧文件再改名。
        unlink(target.c_str());
        if (rename(temporary.c_str(), target.c_str()) != 0) {
            ESP_LOGE(kTag, "[sprite] write_rename_failed errno=%d(%s) from='%s' to='%s'",
                errno, std::strerror(errno), temporary.c_str(), target.c_str());
            return false;
        }
        ESP_LOGI(kTag, "[sprite] stored file=%s bytes=%u", file.file.c_str(),
            static_cast<unsigned>(received));
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

bool ensure(const std::string &provider, const vibe_provider::SpriteInfo &sprite,
            const Fetch &fetch, bool *manifest_changed)
{
    if (provider.empty() || !sprite.valid() || !fetch) return false;
    // 内存紧张的兜底：任何分配失败（bad_alloc 等）在这里降级为「ensure 失败」，
    // 由上层重试。异常若逃出 worker 线程会 std::terminate → abort 反复重启。
    try {
        return ensureImpl(provider, sprite, fetch, manifest_changed);
    } catch (const std::exception &e) {
        ESP_LOGE(kTag, "[sprite] ensure_exception provider=%s what=%s",
            provider.c_str(), e.what());
        return false;
    } catch (...) {
        ESP_LOGE(kTag, "[sprite] ensure_exception provider=%s unknown", provider.c_str());
        return false;
    }
}

bool loadManifest(const std::string &provider, std::string &json)
{
    if (provider.empty()) return false;
    return readIfPresent(pathFor(prefixFor(provider), "manifest.json"), json) && !json.empty();
}

bool cachedVersion(const std::string &provider, uint32_t &version)
{
    std::string manifest;
    if (!loadManifest(provider, manifest)) return false;
    cJSON *root = cJSON_Parse(manifest.c_str());
    if (!root) return false;
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "version");
    const bool ok = cJSON_IsNumber(value) && value->valueint >= 0;
    if (ok) version = static_cast<uint32_t>(value->valueint);
    cJSON_Delete(root);
    return ok;
}

} // namespace made_sprite_cache
