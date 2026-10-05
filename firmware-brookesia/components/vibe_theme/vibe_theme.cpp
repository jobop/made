#include "vibe_theme.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#include "vibe_pairing.hpp"
#include "vibe_usb.hpp"

namespace vibe_theme {
namespace {

constexpr const char *kTag = "vibe_theme";
constexpr const char *kNvsNamespace = "vibe_theme";
constexpr const char *kIconPath = "/spiffs/theme_icon.bin";
constexpr const char *kBgPath = "/spiffs/theme_bg.bin";
constexpr size_t kUsbResponseLimit = 256 * 1024;  // 与 vibe_usb kMaxResponse 对齐

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

Palette g_palette = defaults();
LockAsset g_lock;
std::string g_active;
std::mutex g_sync_mutex;
SyncStatus g_sync;

// ---- NVS 缓存 ----

bool loadPaletteNvs(Palette &out) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = sizeof(Palette);
    const bool ok = nvs_get_blob(handle, "palette", &out, &length) == ESP_OK &&
                    length == sizeof(Palette);
    nvs_close(handle);
    return ok;
}

bool savePaletteNvs(const Palette &palette) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(handle, "palette", &palette, sizeof(palette));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool saveActiveNvs(const std::string &name) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = name.empty() ? nvs_erase_key(handle, "active")
                                    : nvs_set_str(handle, "active", name.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

std::string loadActiveNvs() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return "";
    char name[48] = {};
    size_t length = sizeof(name);
    std::string out;
    if (nvs_get_str(handle, "active", name, &length) == ESP_OK) out = name;
    nvs_close(handle);
    return out;
}

uint16_t loadDimsNvs(const char *key) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return 0;
    uint16_t value = 0;
    nvs_get_u16(handle, key, &value);
    nvs_close(handle);
    return value;
}

bool saveDimsNvs(const char *key, uint16_t value) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_u16(handle, key, value);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

// ---- 锁屏资产（扁平文件 + NVS 尺寸）----

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

// 从扁平文件读入 PSRAM。宽或高为 0 表示该资产未启用。
void loadFlatAsset(const char *path, const char *w_key, const char *h_key,
                   bool &has_flag, uint8_t *&data, uint16_t &w, uint16_t &h) {
    const uint16_t width = loadDimsNvs(w_key);
    const uint16_t height = loadDimsNvs(h_key);
    if (width == 0 || height == 0) return;
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return;
    const size_t expected = static_cast<size_t>(width) * height * 2;
    auto *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(expected, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        fclose(file);
        return;
    }
    const size_t read = fread(buffer, 1, expected, file);
    fclose(file);
    if (read != expected) {
        heap_caps_free(buffer);
        return;
    }
    has_flag = true;
    data = buffer;
    w = width;
    h = height;
    ESP_LOGI(kTag, "Lock asset %s: %ux%u", path, width, height);
}

void loadLockAssets() {
    freeLockAsset();
    loadFlatAsset(kIconPath, "icon_w", "icon_h", g_lock.has_icon, g_lock.icon_data,
                  g_lock.icon_w, g_lock.icon_h);
    loadFlatAsset(kBgPath, "bg_w", "bg_h", g_lock.has_bg, g_lock.bg_data,
                  g_lock.bg_w, g_lock.bg_h);
}

// 一次性清理旧版按主题目录存放的对象（themes/... 前缀）。
void cleanupLegacyThemeFiles() {
    DIR *dir = opendir("/spiffs");
    if (dir == nullptr) return;
    std::vector<std::string> stale;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.rfind("themes/", 0) == 0) stale.push_back("/spiffs/" + name);
    }
    closedir(dir);
    for (const auto &path : stale) {
        if (unlink(path.c_str()) != 0) ESP_LOGW(kTag, "legacy unlink %s failed", path.c_str());
    }
    if (!stale.empty()) ESP_LOGI(kTag, "Cleaned %zu legacy theme files", stale.size());
}

uint32_t colorFromJson(const cJSON *colors, const char *key, uint32_t fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(colors, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) return fallback;
    return static_cast<uint32_t>(std::strtoul(item->valuestring, nullptr, 16));
}

// ---- 传输：base_url 为空 = USB 串口帧；否则 HTTP ----

bool fetchPath(const std::string &base_url, const std::string &path, std::string &out) {
    if (base_url.empty()) {
        const auto pairing = vibe_pairing::snapshot();
        if (pairing.token.empty()) return false;
        int status = 0;
        return vibe_usb::request(path, false, "Bearer " + pairing.token, "", nullptr, 0,
                                 out, status, kUsbResponseLimit, 30000) == ESP_OK &&
               status >= 200 && status < 300;
    }
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
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

} // namespace

esp_err_t init() {
    cleanupLegacyThemeFiles();
    g_active = loadActiveNvs();
    if (!loadPaletteNvs(g_palette)) g_palette = defaults();
    loadLockAssets();
    return ESP_OK;
}

const Palette &palette() { return g_palette; }
const LockAsset &lockAsset() { return g_lock; }
std::string activeName() { return g_active; }

esp_err_t fetchThemeList(const std::string &base_url, std::vector<ThemeInfo> &out) {
    std::string body;
    if (!fetchPath(base_url, "/api/themes", body)) return ESP_FAIL;
    cJSON *list = cJSON_Parse(body.c_str());
    if (list == nullptr) return ESP_ERR_INVALID_STATE;
    const int count = cJSON_GetArraySize(list);
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(list, i);
        const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *title_item = cJSON_GetObjectItemCaseSensitive(item, "title");
        if (!cJSON_IsString(name_item) || name_item->valuestring == nullptr) continue;
        ThemeInfo info{name_item->valuestring, name_item->valuestring};
        if (cJSON_IsString(title_item) && title_item->valuestring != nullptr &&
            title_item->valuestring[0] != 0)
            info.title = title_item->valuestring;
        out.push_back(std::move(info));
    }
    cJSON_Delete(list);
    return out.empty() ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t applyFromBridge(const std::string &name, const std::string &base_url) {
    {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        if (g_sync.state == SyncState::Running) return ESP_ERR_INVALID_STATE;
        g_sync = {SyncState::Running, name.empty() ? "恢复默认主题…" : "获取主题数据…"};
    }
    auto finish = [&](SyncState state, const std::string &message) {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync = {state, message};
    };

    if (name.empty()) {
        g_palette = defaults();
        freeLockAsset();
        unlink(kIconPath);
        unlink(kBgPath);
        saveDimsNvs("icon_w", 0);
        saveDimsNvs("icon_h", 0);
        saveDimsNvs("bg_w", 0);
        saveDimsNvs("bg_h", 0);
        if (!saveActiveNvs("")) {
            finish(SyncState::Failed, "写入缓存失败");
            return ESP_FAIL;
        }
        finish(SyncState::Done, "已恢复默认主题，正在重启…");
        return ESP_OK;
    }

    // 1) 拉取 theme.json。
    std::string json;
    if (!fetchPath(base_url, "/api/themes/" + name + "/files/theme.json", json)) {
        finish(SyncState::Failed, "无法获取主题数据");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        finish(SyncState::Failed, "主题数据格式错误");
        return ESP_ERR_INVALID_STATE;
    }
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

    // 2) 先全部拉进内存（PSRAM），全部成功才落盘，避免半套缓存。
    struct AssetBlob {
        std::vector<uint8_t> data;
        uint16_t w = 0, h = 0;
        const char *file_key, *w_key, *h_key, *path, *nvs_w, *nvs_h;
    };
    AssetBlob assets[] = {
        {{}, 0, 0, "icon", "icon_w", "icon_h", kIconPath, "icon_w", "icon_h"},
        {{}, 0, 0, "bg", "bg_w", "bg_h", kBgPath, "bg_w", "bg_h"},
    };
    const cJSON *lock = cJSON_GetObjectItemCaseSensitive(root, "lock");
    if (cJSON_IsObject(lock)) {
        for (auto &asset : assets) {
            const cJSON *file_item = cJSON_GetObjectItemCaseSensitive(lock, asset.file_key);
            const cJSON *w_item = cJSON_GetObjectItemCaseSensitive(lock, asset.w_key);
            const cJSON *h_item = cJSON_GetObjectItemCaseSensitive(lock, asset.h_key);
            if (!cJSON_IsString(file_item) || file_item->valuestring == nullptr ||
                !cJSON_IsNumber(w_item) || !cJSON_IsNumber(h_item))
                continue;
            asset.w = static_cast<uint16_t>(w_item->valuedouble);
            asset.h = static_cast<uint16_t>(h_item->valuedouble);
            if (asset.w == 0 || asset.h == 0 || asset.w > 480 || asset.h > 480) {
                asset.w = asset.h = 0;
                continue;
            }
            std::lock_guard<std::mutex> progress_lock(g_sync_mutex);
            g_sync.message = std::string("下载 ") + asset.file_key + "…";
            std::string content;
            if (!fetchPath(base_url, "/api/themes/" + name + "/files/" + file_item->valuestring,
                           content) ||
                content.size() != static_cast<size_t>(asset.w) * asset.h * 2) {
                cJSON_Delete(root);
                finish(SyncState::Failed, "主题资产下载失败");
                return ESP_FAIL;
            }
            asset.data.assign(content.begin(), content.end());
        }
    }
    cJSON_Delete(root);

    // 3) 落盘 + 提交 NVS。
    for (auto &asset : assets) {
        if (asset.data.empty()) {
            unlink(asset.path);
            saveDimsNvs(asset.nvs_w, 0);
            saveDimsNvs(asset.nvs_h, 0);
            continue;
        }
        FILE *file = fopen(asset.path, "wb");
        if (file == nullptr) {
            finish(SyncState::Failed, "写入缓存失败");
            return ESP_FAIL;
        }
        fwrite(asset.data.data(), 1, asset.data.size(), file);
        fclose(file);
        saveDimsNvs(asset.nvs_w, asset.w);
        saveDimsNvs(asset.nvs_h, asset.h);
    }
    if (!savePaletteNvs(next) || !saveActiveNvs(name)) {
        finish(SyncState::Failed, "写入缓存失败");
        return ESP_FAIL;
    }
    finish(SyncState::Done, "已应用，正在重启…");
    return ESP_OK;
}

SyncStatus syncStatus() {
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    return g_sync;
}

} // namespace vibe_theme
