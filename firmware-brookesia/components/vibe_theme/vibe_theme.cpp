#include "vibe_theme.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdlib>
#include <functional>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#include "vibe_pairing.hpp"
#include "vibe_usb.hpp"

namespace vibe_theme {
namespace {

constexpr const char *kTag = "vibe_theme";
constexpr const char *kRoot = "/spiffs/themes";
constexpr const char *kNvsNamespace = "vibe_theme";
constexpr const char *kNvsKey = "active";
constexpr size_t kUsbResponseLimit = 256 * 1024;  // 与 vibe_usb kMaxResponse 对齐（锁屏背景最大 ~150KB）

Palette defaults() {
    return Palette{
        .accent = 0x1C856F,
        .btn_normal = 0x284668,
        .btn_cancel = 0x42566B,
        .border = 0x83B5E6,
        .text_primary = 0xF1F5FF,
        .text_secondary = 0xB8C9E4,
        .text_hint = 0x9FADD0,
        .text_accent = 0x8CE4CB,
        .title = 0xE7EEFF,
        .bg_page = 0x091321,
        .bg_panel = 0x0B1830,
        .bg_row = 0x10243B,
        .danger = 0x9C4856,
        .lock_bg = 0x0B1518,
    };
}

// 主题线程读、LVGL 线程读：字段只在 apply/init（重启前）写入。
Palette g_palette = defaults();
LockAsset g_lock;
std::string g_active;
std::mutex g_sync_mutex;
SyncStatus g_sync;

std::string themeDir(const std::string &name) {
    return std::string(kRoot) + "/" + name;
}

bool readTextFile(const std::string &path, std::string &out) {
    FILE *file = fopen(path.c_str(), "r");
    if (file == nullptr) return false;
    out.clear();
    char buffer[512];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, got);
    fclose(file);
    return true;
}

uint32_t colorFromJson(const cJSON *colors, const char *key, uint32_t fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(colors, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return fallback;
    return static_cast<uint32_t>(std::strtoul(item->valuestring, nullptr, 16));
}

void freeLockAsset() {
    if (g_lock.icon_data) {
        heap_caps_free(g_lock.icon_data);
        g_lock.icon_data = nullptr;
    }
    if (g_lock.bg_data) {
        heap_caps_free(g_lock.bg_data);
        g_lock.bg_data = nullptr;
    }
    g_lock = LockAsset{};
}

// 读取 lock 段描述的 RGB565 资产到 PSRAM。
void loadLockAsset(const std::string &dir, const cJSON *lock) {
    freeLockAsset();
    if (!cJSON_IsObject(lock)) return;
    struct Entry {
        const char *file_key, *w_key, *h_key;
        bool &has_flag;
        uint8_t *&data;
        uint16_t &w, &h;
    } entries[] = {
        {"icon", "icon_w", "icon_h", g_lock.has_icon, g_lock.icon_data, g_lock.icon_w, g_lock.icon_h},
        {"bg", "bg_w", "bg_h", g_lock.has_bg, g_lock.bg_data, g_lock.bg_w, g_lock.bg_h},
    };
    for (auto &entry : entries) {
        const cJSON *file_item = cJSON_GetObjectItemCaseSensitive(lock, entry.file_key);
        if (!cJSON_IsString(file_item) || file_item->valuestring == nullptr) continue;
        const cJSON *w_item = cJSON_GetObjectItemCaseSensitive(lock, entry.w_key);
        const cJSON *h_item = cJSON_GetObjectItemCaseSensitive(lock, entry.h_key);
        if (!cJSON_IsNumber(w_item) || !cJSON_IsNumber(h_item)) continue;
        const int width = static_cast<int>(w_item->valuedouble);
        const int height = static_cast<int>(h_item->valuedouble);
        if (width <= 0 || height <= 0 || width > 480 || height > 480) continue;
        FILE *file = fopen((dir + "/" + file_item->valuestring).c_str(), "rb");
        if (file == nullptr) continue;
        const size_t expected = static_cast<size_t>(width) * height * 2;
        uint8_t *buffer = static_cast<uint8_t *>(
            heap_caps_malloc(expected, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buffer == nullptr) {
            fclose(file);
            continue;
        }
        const size_t read = fread(buffer, 1, expected, file);
        fclose(file);
        if (read != expected) {
            heap_caps_free(buffer);
            continue;
        }
        entry.has_flag = true;
        entry.data = buffer;
        entry.w = static_cast<uint16_t>(width);
        entry.h = static_cast<uint16_t>(height);
        ESP_LOGI(kTag, "Lock asset %s: %dx%d", entry.file_key, width, height);
    }
}

} // namespace

esp_err_t init() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        char name[48] = {};
        size_t length = sizeof(name);
        if (nvs_get_str(handle, kNvsKey, name, &length) == ESP_OK && name[0] != 0) {
            g_active = name;
        }
        nvs_close(handle);
    }
    if (g_active.empty()) return ESP_OK;
    const esp_err_t result = apply(g_active);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Theme '%s' unavailable, falling back to default", g_active.c_str());
        g_active.clear();
        g_palette = defaults();
        freeLockAsset();
    }
    return ESP_OK;
}

const Palette &palette() { return g_palette; }
const LockAsset &lockAsset() { return g_lock; }
std::string activeName() { return g_active; }

std::vector<ThemeInfo> list() {
    std::vector<ThemeInfo> out;
    DIR *dir = opendir(kRoot);
    if (dir == nullptr) return out;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.') continue;
        const std::string name = entry->d_name;
        std::string json;
        if (!readTextFile(themeDir(name) + "/theme.json", json)) continue;
        ThemeInfo info{name, name};
        cJSON *root = cJSON_Parse(json.c_str());
        if (root != nullptr) {
            const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
            if (cJSON_IsString(title) && title->valuestring != nullptr && title->valuestring[0] != 0)
                info.title = title->valuestring;
            cJSON_Delete(root);
        }
        out.push_back(std::move(info));
    }
    closedir(dir);
    return out;
}

esp_err_t apply(const std::string &name) {
    if (name.empty()) {
        // 恢复默认。
        g_palette = defaults();
        freeLockAsset();
        g_active.clear();
        nvs_handle_t handle;
        if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_key(handle, kNvsKey);
            nvs_commit(handle);
            nvs_close(handle);
        }
        return ESP_OK;
    }
    const std::string dir = themeDir(name);
    std::string json;
    if (!readTextFile(dir + "/theme.json", json)) {
        ESP_LOGW(kTag, "theme.json missing for '%s'", name.c_str());
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *root = cJSON_Parse(json.c_str());
    if (root == nullptr) return ESP_ERR_INVALID_STATE;
    Palette next = defaults();
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(root, "colors");
    if (cJSON_IsObject(colors)) {
        next.accent = colorFromJson(colors, "accent", next.accent);
        next.btn_normal = colorFromJson(colors, "btn_normal", next.btn_normal);
        next.btn_cancel = colorFromJson(colors, "btn_cancel", next.btn_cancel);
        next.border = colorFromJson(colors, "border", next.border);
        next.text_primary = colorFromJson(colors, "text_primary", next.text_primary);
        next.text_secondary = colorFromJson(colors, "text_secondary", next.text_secondary);
        next.text_hint = colorFromJson(colors, "text_hint", next.text_hint);
        next.text_accent = colorFromJson(colors, "text_accent", next.text_accent);
        next.title = colorFromJson(colors, "title", next.title);
        next.bg_page = colorFromJson(colors, "bg_page", next.bg_page);
        next.bg_panel = colorFromJson(colors, "bg_panel", next.bg_panel);
        next.bg_row = colorFromJson(colors, "bg_row", next.bg_row);
        next.danger = colorFromJson(colors, "danger", next.danger);
        next.lock_bg = colorFromJson(colors, "lock_bg", next.lock_bg);
    }
    g_palette = next;
    loadLockAsset(dir, cJSON_GetObjectItemCaseSensitive(root, "lock"));
    cJSON_Delete(root);
    g_active = name;
    nvs_handle_t handle;
    esp_err_t result = ESP_OK;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        result = nvs_set_str(handle, kNvsKey, name.c_str());
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(kTag, "Theme '%s' applied (reboot to take effect)", name.c_str());
    return result;
}

esp_err_t remove(const std::string &name) {
    if (name.empty() || name.find("..") != std::string::npos || name.find('/') != std::string::npos)
        return ESP_ERR_INVALID_ARG;
    const std::string dir = themeDir(name);
    DIR *handle = opendir(dir.c_str());
    if (handle == nullptr) return ESP_ERR_NOT_FOUND;
    struct dirent *entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        const std::string file = dir + "/" + entry->d_name;
        if (unlink(file.c_str()) != 0) ESP_LOGW(kTag, "unlink %s failed", file.c_str());
    }
    closedir(handle);
    unlink(dir.c_str());  // SPIFFS VFS 以空目录文件模拟目录
    if (g_active == name) (void)apply("");
    return ESP_OK;
}

esp_err_t saveFile(const std::string &theme, const std::string &filename,
                   const uint8_t *data, size_t size) {
    if (theme.empty() || theme.find("..") != std::string::npos || theme.find('/') != std::string::npos)
        return ESP_ERR_INVALID_ARG;
    if (filename.empty() || filename.find("..") != std::string::npos) return ESP_ERR_INVALID_ARG;
    const std::string dir = themeDir(theme);
    mkdir(kRoot, 0775);
    mkdir(dir.c_str(), 0775);
    FILE *file = fopen((dir + "/" + filename).c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGE(kTag, "fopen %s/%s failed", theme.c_str(), filename.c_str());
        return ESP_FAIL;
    }
    const size_t written = fwrite(data, 1, size, file);
    fclose(file);
    return written == size ? ESP_OK : ESP_FAIL;
}

SyncStatus syncStatus() {
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    return g_sync;
}

// 传输抽象：GET 一个路径，返回响应体。HTTP（Wi-Fi/接收器）与 USB 串口帧两种实现。
using Fetcher = std::function<bool(const std::string &path, std::string &out)>;

// 共享安装流程：列表 -> 清单 -> 逐文件落盘。
int installThemes(const Fetcher &fetch) {
    std::string body;
    if (!fetch("/api/themes", body) || body.empty()) return -1;
    cJSON *list = cJSON_Parse(body.c_str());
    if (list == nullptr) return -1;
    const int count = cJSON_GetArraySize(list);
    int installed = 0;
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(list, i);
        const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(name_item) || name_item->valuestring == nullptr) continue;
        const std::string name = name_item->valuestring;
        {
            std::lock_guard<std::mutex> lock(g_sync_mutex);
            g_sync.message = "下载 " + name + "…";
        }
        std::string manifest;
        if (!fetch("/api/themes/" + name, manifest)) continue;
        cJSON *manifest_json = cJSON_Parse(manifest.c_str());
        const cJSON *files = manifest_json == nullptr ? nullptr
                                                      : cJSON_GetObjectItemCaseSensitive(manifest_json, "files");
        if (!cJSON_IsArray(files)) {
            if (manifest_json != nullptr) cJSON_Delete(manifest_json);
            continue;
        }
        bool ok = true;
        const int file_count = cJSON_GetArraySize(files);
        for (int f = 0; f < file_count && ok; ++f) {
            const cJSON *file_item = cJSON_GetArrayItem(files, f);
            if (!cJSON_IsString(file_item) || file_item->valuestring == nullptr) continue;
            const std::string filename = file_item->valuestring;
            std::string content;
            if (!fetch("/api/themes/" + name + "/files/" + filename, content) ||
                content.empty() ||
                saveFile(name, filename,
                         reinterpret_cast<const uint8_t *>(content.data()), content.size()) != ESP_OK) {
                ok = false;
            }
        }
        if (manifest_json != nullptr) cJSON_Delete(manifest_json);
        if (ok) ++installed;
    }
    cJSON_Delete(list);
    return installed;
}

bool httpFetch(const std::string &base, const std::string &path, std::string &out) {
    const std::string url = base + path;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return false;
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        char buffer[1024];
        int read = 0;
        while ((read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0)
            out.append(buffer, read);
        esp_http_client_close(client);
        ok = true;
    }
    esp_http_client_cleanup(client);
    return ok;
}

bool usbFetch(const std::string &path, std::string &out) {
    const auto pairing = vibe_pairing::snapshot();
    if (pairing.token.empty()) return false;
    const std::string authorization = "Bearer " + pairing.token;
    int status = 0;
    return vibe_usb::request(path, false, authorization, "", nullptr, 0,
                             out, status, kUsbResponseLimit, 30000) == ESP_OK &&
           status >= 200 && status < 300;
}

esp_err_t syncFromBridge(const std::string &base_url) {
    {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        if (g_sync.state == SyncState::Running) return ESP_ERR_INVALID_STATE;
        g_sync = {SyncState::Running, "连接电脑…"};
    }
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const int installed = installThemes([&base](const std::string &path, std::string &out) {
        return httpFetch(base, path, out);
    });
    if (installed > 0) {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync = {SyncState::Done, "已安装 " + std::to_string(installed) + " 个主题"};
        return ESP_OK;
    }
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    g_sync = {SyncState::Failed, "无法连接电脑或没有新主题"};
    return ESP_FAIL;
}

esp_err_t syncOverUsb() {
    {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        if (g_sync.state == SyncState::Running) return ESP_ERR_INVALID_STATE;
        g_sync = {SyncState::Running, "通过 USB 同步…"};
    }
    const int installed = installThemes([](const std::string &path, std::string &out) {
        return usbFetch(path, out);
    });
    if (installed > 0) {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync = {SyncState::Done, "已安装 " + std::to_string(installed) + " 个主题"};
        return ESP_OK;
    }
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    g_sync = {SyncState::Failed, "USB 同步失败：未配对或电脑上没有新主题"};
    return ESP_FAIL;
}

} // namespace vibe_theme
