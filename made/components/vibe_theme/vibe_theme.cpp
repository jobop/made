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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "vibe_pairing.hpp"
#include "vibe_wifi.hpp"
#include "vibe_usb.hpp"

namespace vibe_theme {
namespace {

constexpr const char *kTag = "vibe_theme";
constexpr const char *kNvsNamespace = "vibe_theme";
constexpr const char *kSoundPath = "/spiffs/theme_chime.wav";
constexpr size_t kMaxSoundBytes = 256 * 1024;  // USB 帧转发单响应上限
constexpr size_t kUsbResponseLimit = 256 * 1024;  // 与 vibe_usb kMaxResponse 对齐

// 内置默认主题：莫兰迪灰调（暖燕麦底 + 灰绿点缀，整体低饱和不刺眼）。
// 对比度按 puhui 16px 点阵小字核算：正文 ≥7:1，提示 ≥3.5:1。
Palette defaults() {
    return Palette{
        .accent = 0x6F8F87,
        .btn_normal = 0xE2DFD8,
        .btn_cancel = 0xE9E6E0,
        .border = 0xD7D3CA,
        .text_primary = 0x3B403D,
        .text_secondary = 0x6C736E,
        .text_hint = 0x8D938C,
        .text_accent = 0x5A7A72,
        .text_answer = 0x737C76,
        .text_on_accent = 0xF7F6F2,
        .title = 0x333936,
        .bg_page = 0xEFECE6,
        .bg_panel = 0xF7F5F1,
        .bg_row = 0xE4E1DA,
        .home_bg = 0xE3E5DC,
        .danger = 0xA5676E,
        .chip_accent = 0xDDE3DD,
        .danger_soft = 0xE9DEDC,
        .warning = 0xC09566,
        .success = 0x7E9B84,
    };
}

Palette g_palette = defaults();
struct Metrics {
    int radius_scale = 100;  // 50~200
    int shape = kShapeRounded;
    int base = 1;            // 1 = light（内置默认浅色）
    int font_px = 16;        // 16 / 20
};
Metrics g_metrics;
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

// ---- 风格参数（圆角缩放 / 形状 / 亮暗 / 字号档）----

void loadMetricsNvs() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint16_t value = 0;
    if (nvs_get_u16(handle, "radius_scale", &value) == ESP_OK && value >= 50 && value <= 200)
        g_metrics.radius_scale = value;
    value = 0;
    if (nvs_get_u16(handle, "shape", &value) == ESP_OK && value <= kShapeSquare)
        g_metrics.shape = value;
    value = 0;
    if (nvs_get_u16(handle, "base", &value) == ESP_OK && value <= 1)
        g_metrics.base = value;
    value = 0;
    if (nvs_get_u16(handle, "font_px", &value) == ESP_OK && (value == 16 || value == 20))
        g_metrics.font_px = value;
    nvs_close(handle);
}

bool saveMetricsNvs() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_u16(handle, "radius_scale", g_metrics.radius_scale);
    if (result == ESP_OK) result = nvs_set_u16(handle, "shape", g_metrics.shape);
    if (result == ESP_OK) result = nvs_set_u16(handle, "base", g_metrics.base);
    if (result == ESP_OK) result = nvs_set_u16(handle, "font_px", g_metrics.font_px);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool soundEnabledNvs() { return loadDimsNvs("has_sound") == 1; }

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

bool fetchPath(const std::string &base_url, const std::string &path, std::string &out,
               const char *progress_label = nullptr);

bool fetchPathRetry(const std::string &base_url, const std::string &path, std::string &out,
                    const char *progress_label = nullptr) {
    // 接收器中继一次只转一个请求，与设备自身的心跳/轮询竞争；失败重试 3 次。
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(400));
        std::string result;
        if (fetchPath(base_url, path, result, progress_label)) {
            out = std::move(result);
            return true;
        }
        ESP_LOGW(kTag, "[retry] path=%s 第 %d 次尝试失败", path.c_str(), attempt + 1);
    }
    ESP_LOGE(kTag, "[retry] path=%s 三次尝试均失败", path.c_str());
    return false;
}

// 网络模式（非 USB）先等 WiFi 就绪：开机到连上接收器/路由器需要 15~20s，
// 过早发起请求必然失败（实测 TCP 直接被拒）。
void waitForNetwork(const std::string &base_url) {
    if (base_url.empty()) return;  // USB 模式无网络等待
    for (int waited = 0; waited < 20000 && !vibe_wifi::station_connected(); waited += 400) {
        {
            std::lock_guard<std::mutex> lock(g_sync_mutex);
            g_sync = {SyncState::Running, "等待网络连接…"};
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

bool fetchPath(const std::string &base_url, const std::string &path, std::string &out,
               const char *progress_label) {
    const int64_t started = esp_timer_get_time();
    if (base_url.empty()) {
        const auto pairing = vibe_pairing::snapshot();
        if (pairing.token.empty()) {
            ESP_LOGW(kTag, "[fetch] USB 无配对令牌 path=%s", path.c_str());
            return false;
        }
        int status = 0;
        const esp_err_t result = vibe_usb::request(path, false, "Bearer " + pairing.token, "",
                                                   nullptr, 0, out, status, kUsbResponseLimit,
                                                   30000);
        ESP_LOGI(kTag, "[fetch] USB path=%s result=%s status=%d bytes=%zu %.1fs",
                 path.c_str(), esp_err_to_name(result), status, out.size(),
                 (esp_timer_get_time() - started) / 1000000.0);
        return result == ESP_OK && status >= 200 && status < 300;
    }
    if (vibe_usb::is_receiver_url(base_url)) {
        const auto pairing = vibe_pairing::snapshot();
        if (pairing.token.empty()) {
            ESP_LOGW(kTag, "[fetch] 接收端无配对令牌 path=%s", path.c_str());
            return false;
        }
        if (progress_label != nullptr) {
            std::lock_guard<std::mutex> lock(g_sync_mutex);
            g_sync.message = progress_label;
        }
        int status = 0;
        const esp_err_t result = vibe_usb::receiver_request(
            path, false, "Bearer " + pairing.token, "", nullptr, 0, out, status,
            kUsbResponseLimit, 35000);
        ESP_LOGI(kTag, "[fetch] 接收端 path=%s result=%s status=%d bytes=%zu %.1fs",
                 path.c_str(), esp_err_to_name(result), status, out.size(),
                 (esp_timer_get_time() - started) / 1000000.0);
        return result == ESP_OK && status >= 200 && status < 300;
    }
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string url = base + path;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    // 大文件（提示音 ~224KB）经接收器串口中继需 10s+；须长于接收器的 30s 转发超时。
    config.timeout_ms = 35000;
    if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(kTag, "[fetch] HTTP 客户端初始化失败 url=%s", url.c_str());
        return false;
    }
    esp_http_client_set_header(client, "Connection", "close");
    bool ok = false;
    int http_status = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        const int64_t total = esp_http_client_fetch_headers(client) > 0
                                  ? esp_http_client_get_content_length(client)
                                  : -1;
        char buffer[2048];
        int read = 0;
        size_t received = 0;
        int last_bucket = -1;
        while ((read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
            out.append(buffer, read);
            received += static_cast<size_t>(read);
            if (progress_label != nullptr && total > 0) {
                const int bucket = static_cast<int>(received * 100 / static_cast<size_t>(total)) / 10;
                if (bucket != last_bucket) {
                    last_bucket = bucket;
                    std::lock_guard<std::mutex> lock(g_sync_mutex);
                    g_sync.message = std::string(progress_label) + " " +
                                     std::to_string(bucket * 10) + "%";
                }
            }
        }
        http_status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        // 截断检测：中继链断流时连接提前关闭，收不满 Content-Length。
        if (total > 0 && received != static_cast<size_t>(total)) {
            ESP_LOGW(kTag, "[fetch] HTTP truncated path=%s got=%zu want=%lld",
                     path.c_str(), received, (long long)total);
            ok = false;
        } else {
            ok = true;
        }
    } else {
        ESP_LOGE(kTag, "[fetch] HTTP open 失败 url=%s", url.c_str());
    }
    esp_http_client_cleanup(client);
    ESP_LOGI(kTag, "[fetch] HTTP path=%s ok=%d status=%d bytes=%zu %.1fs",
             path.c_str(), ok, http_status, out.size(),
             (esp_timer_get_time() - started) / 1000000.0);
    return ok;
}

} // namespace

esp_err_t init() {
    cleanupLegacyThemeFiles();
    g_active = loadActiveNvs();
    if (!loadPaletteNvs(g_palette)) g_palette = defaults();
    loadMetricsNvs();
    return ESP_OK;
}

const Palette &palette() { return g_palette; }
int radiusScale() { return g_metrics.radius_scale; }
int shapeMode() { return g_metrics.shape; }
bool isLightBase() { return g_metrics.base == 1; }
int fontPx() { return g_metrics.font_px; }
std::string soundPath() { return soundEnabledNvs() ? std::string(kSoundPath) : std::string(); }
std::string activeName() { return g_active; }

esp_err_t fetchThemeList(const std::string &base_url, std::vector<ThemeInfo> &out) {
    waitForNetwork(base_url);
    {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync = {SyncState::Running, base_url.empty() ? "经 USB 获取列表…" : "经网络获取列表…"};
    }
    std::string body;
    const esp_err_t fetch_result = fetchPathRetry(base_url, "/api/themes", body, "获取列表") ? ESP_OK : ESP_FAIL;
    if (fetch_result != ESP_OK) {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync = {SyncState::Failed, base_url.empty()
                                            ? "USB 获取列表失败（未配对或未选 USB 直连）"
                                            : "网络获取列表失败，请点重试"};
        return fetch_result;
    }
    cJSON *list = cJSON_Parse(body.c_str());
    if (list == nullptr) {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync = {SyncState::Failed, "电脑返回格式错误"};
        return ESP_ERR_INVALID_STATE;
    }
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
    ESP_LOGI(kTag, "[list] 桥接器返回 %zu 个主题", out.size());
    if (out.empty()) {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync = {SyncState::Failed, "电脑上还没有主题包"};
        return ESP_ERR_NOT_FOUND;
    }
    {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync = {SyncState::Done, ""};
    }
    return ESP_OK;
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
    waitForNetwork(base_url);

    if (name.empty()) {
        g_palette = defaults();
        g_metrics = Metrics{};
        savePaletteNvs(g_palette);  // 必须同步覆盖 NVS 调色板，否则重启后又加载回来
        saveMetricsNvs();
        unlink(kSoundPath);
        saveDimsNvs("has_sound", 0);
        if (!saveActiveNvs("")) {
            finish(SyncState::Failed, "写入缓存失败");
            return ESP_FAIL;
        }
        finish(SyncState::Done, "已恢复默认主题，正在重启…");
        return ESP_OK;
    }

    // 1) 拉取 theme.json。
    std::string json;
    if (!fetchPathRetry(base_url, "/api/themes/" + name + "/files/theme.json", json, "主题数据")) {
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
        next.text_answer = colorFromJson(colors, "text_answer", next.text_answer);
        next.text_on_accent = colorFromJson(colors, "text_on_accent", next.text_on_accent);
        next.title = colorFromJson(colors, "title", next.title);
        next.bg_page = colorFromJson(colors, "bg_page", next.bg_page);
        next.bg_panel = colorFromJson(colors, "bg_panel", next.bg_panel);
        next.bg_row = colorFromJson(colors, "bg_row", next.bg_row);
        next.home_bg = colorFromJson(colors, "home_bg", next.home_bg);
        next.danger = colorFromJson(colors, "danger", next.danger);
        next.chip_accent = colorFromJson(colors, "chip_accent", next.chip_accent);
        next.danger_soft = colorFromJson(colors, "danger_soft", next.danger_soft);
        next.warning = colorFromJson(colors, "warning", next.warning);
        next.success = colorFromJson(colors, "success", next.success);
    }

    // 风格参数：亮暗元数据 / 圆角缩放 / 形状语言 / 字号档。
    const cJSON *base_item = cJSON_GetObjectItemCaseSensitive(root, "base");
    if (cJSON_IsString(base_item) && base_item->valuestring != nullptr)
        g_metrics.base = std::string(base_item->valuestring) == "light" ? 1 : 0;
    const cJSON *metrics = cJSON_GetObjectItemCaseSensitive(root, "metrics");
    if (cJSON_IsObject(metrics)) {
        const cJSON *scale = cJSON_GetObjectItemCaseSensitive(metrics, "radius_scale");
        if (cJSON_IsNumber(scale) && scale->valuedouble >= 50 && scale->valuedouble <= 200)
            g_metrics.radius_scale = static_cast<int>(scale->valuedouble);
    }
    const cJSON *shape_item = cJSON_GetObjectItemCaseSensitive(root, "shape");
    if (cJSON_IsString(shape_item) && shape_item->valuestring != nullptr) {
        const std::string shape = shape_item->valuestring;
        if (shape == "circle") g_metrics.shape = kShapeCircle;
        else if (shape == "square") g_metrics.shape = kShapeSquare;
        else g_metrics.shape = kShapeRounded;
    }
    const cJSON *font_item = cJSON_GetObjectItemCaseSensitive(root, "font");
    if (cJSON_IsNumber(font_item) && (font_item->valuedouble == 16 || font_item->valuedouble == 20))
        g_metrics.font_px = static_cast<int>(font_item->valuedouble);

    // 2) 提示音（WAV）：拉进内存，成功才落盘。
    std::vector<uint8_t> sound_data;
    const cJSON *sound = cJSON_GetObjectItemCaseSensitive(root, "sound");
    if (cJSON_IsObject(sound)) {
        const cJSON *file_item = cJSON_GetObjectItemCaseSensitive(sound, "file");
        if (cJSON_IsString(file_item) && file_item->valuestring != nullptr) {
            // 只在改状态时拿锁。下载期间握着锁会挡住界面刷新；失败分支再调
            // finish() 会再次加锁，这把锁不能重入，界面就停在上一句文案。
            {
                std::lock_guard<std::mutex> progress_lock(g_sync_mutex);
                g_sync.message = "下载 提示音…";
            }
            std::string content;
            if (!fetchPath(base_url, "/api/themes/" + name + "/files/" + file_item->valuestring,
                           content, "提示音") ||
                content.empty() || content.size() > kMaxSoundBytes) {
                cJSON_Delete(root);
                finish(SyncState::Failed, "提示音下载失败（≤256KB）");
                return ESP_FAIL;
            }
            sound_data.assign(content.begin(), content.end());
        }
    }

    cJSON_Delete(root);

    // 3) 落盘 + 提交 NVS（锁屏资产已随锁屏功能移除，只保留提示音）。
    if (sound_data.empty()) {
        unlink(kSoundPath);
        saveDimsNvs("has_sound", 0);
    } else {
        FILE *sound_file = fopen(kSoundPath, "wb");
        if (sound_file == nullptr) {
            finish(SyncState::Failed, "写入缓存失败");
            return ESP_FAIL;
        }
        fwrite(sound_data.data(), 1, sound_data.size(), sound_file);
        fclose(sound_file);
        saveDimsNvs("has_sound", 1);
    }
    if (!savePaletteNvs(next) || !saveMetricsNvs() || !saveActiveNvs(name)) {
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
