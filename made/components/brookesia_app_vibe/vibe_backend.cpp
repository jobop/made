// 冒烟测试标准路径开关（唯一源：vibe_usb/include/vibe_smoke_test.h）。
#include "vibe_smoke_test.h"
#include "esp_brookesia_app_vibe.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

#include "board_voice_audio.h"
#include "boot_click_gesture.hpp"
#include "made_layout.hpp"
#include "made_sprite.hpp"
#include "made_sprite_cache.hpp"
#include "new_message_chime.hpp"
#include "vibe_motion.hpp"
#include "cJSON.h"
#include "bsp/display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "mbedtls/md.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "vibe_pairing.hpp"
#include "vibe_voice_capture.hpp"
#include "vibe_wifi.hpp"
#include "vibe_theme.hpp"
#include "vibe_usb.hpp"
#include "vibe_i18n.hpp"
#include "vibe_touch_keyboard.hpp"
#include "vibe_phone_setup.hpp"

using esp_brookesia::systems::phone::App;

LV_IMG_DECLARE(img_app_vibe);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_4);
#include "vibe_ui_shared.hpp"

namespace esp_brookesia::apps {

void VibeCoding::runFirmwareUpdate()
{
    auto note = [this](const std::string &text) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        firmware_status_ = text;
        ++model_.revision;
    };
    struct BusyGuard {
        std::atomic<bool> &flag;
        bool release = true;
        ~BusyGuard() { if (release) flag.store(false); }
    } busy{firmware_busy_};
    firmware_busy_.store(true);
    note(T("正在从配置的地址下载固件…"));
    const std::string version = firmware_version_;
    const std::string sha = firmware_sha_;
    const std::string url = firmware_url_;
    const int size = firmware_size_;
    firmware_trace_stage_ = "pull";
    firmware_trace_size_ = size;
    firmware_trace_written_ = 0;
    firmware_trace_http_ = 0;
    if (url.empty()) {
        firmware_trace_stage_ = "fail";
        firmware_trace_result_ = "empty";
        ESP_LOGW("vibe_fw", "[fw] abort empty-url tasks=%d task=%s", firmware_trace_tasks_, firmware_trace_task_.c_str());
        note(T("电脑上还没有配置固件地址"));
        return;
    }
    std::string bridge_path;
    const std::string base = vibe_pairing::snapshot().url;
    std::string origin = base;
    while (!origin.empty() && origin.back() == '/') origin.pop_back();
    if (url.rfind("/device/firmware/current", 0) == 0) {
        bridge_path = "/device/firmware/current";
    } else if (!origin.empty() && url.rfind(origin, 0) == 0) {
        std::string rest = url.substr(origin.size());
        const auto query = rest.find('?');
        if (query != std::string::npos) rest.resize(query);
        if (rest == "/device/firmware/current") bridge_path = rest;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        firmware_trace_stage_ = "fail";
        firmware_trace_result_ = "partition";
        ESP_LOGW("vibe_fw", "[fw] abort no-partition why=%s", firmware_trace_why_.c_str());
        note(T("没有可用的更新分区"));
        return;
    }
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, size > 0 ? static_cast<size_t>(size) : OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        firmware_trace_stage_ = "fail";
        firmware_trace_result_ = "begin";
        ESP_LOGW("vibe_fw", "[fw] abort begin size=%d", size);
        note(T("无法开始写入固件"));
        return;
    }
    ESP_LOGW("vibe_fw", "[fw] pull why=%s tasks=%d task=%s size=%d bridge=%d url=%.160s",
        firmware_trace_why_.c_str(), firmware_trace_tasks_, firmware_trace_task_.c_str(), size,
        bridge_path.empty() ? 0 : 1, url.c_str());
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool hash_ok = info && mbedtls_md_setup(&md, info, 0) == 0 && mbedtls_md_starts(&md) == 0;
    auto fail = [&](const char *text) {
        mbedtls_md_free(&md);
        if (handle) esp_ota_abort(handle);
        handle = 0;
        note(T(text));
    };
    size_t written = 0;
    bool io_ok = false;
    int last_http = 0;
    auto pullChunks = [&]() {
        if (bridge_path.empty()) return false;
        constexpr size_t kChunk = 128 * 1024;
        while (true) {
            const std::string path = bridge_path + "?offset=" + std::to_string(written) + "&length=" + std::to_string(kChunk);
            std::string chunk;
            int chunk_status = 0;
            const bool got = request(path, false, chunk, chunk_status, "", kChunk + 1024, 20000);
            last_http = chunk_status;
            firmware_trace_http_ = chunk_status;
            firmware_trace_written_ = static_cast<int>(written);
            if (!got || chunk_status != 200) {
                ESP_LOGW("vibe_fw", "[fw] chunk off=%u http=%d got=%d bytes=%u",
                    static_cast<unsigned>(written), chunk_status, got ? 1 : 0, static_cast<unsigned>(chunk.size()));
                return false;
            }
            if (chunk.empty()) return written >= 16;
            if (esp_ota_write(handle, chunk.data(), chunk.size()) != ESP_OK) {
                ESP_LOGW("vibe_fw", "[fw] write off=%u bytes=%u", static_cast<unsigned>(written), static_cast<unsigned>(chunk.size()));
                return false;
            }
            if (hash_ok) mbedtls_md_update(&md, reinterpret_cast<const unsigned char *>(chunk.data()), chunk.size());
            written += chunk.size();
            firmware_trace_written_ = static_cast<int>(written);
            ESP_LOGI("vibe_fw", "[fw] chunk off=%u bytes=%u total=%u",
                static_cast<unsigned>(written - chunk.size()), static_cast<unsigned>(chunk.size()), static_cast<unsigned>(written));
            if (written > 8 * 1024 * 1024) return false;
            if (chunk.size() < kChunk) return true;
            if (size > 0 && written >= static_cast<size_t>(size)) return true;
        }
    };
    if (!bridge_path.empty()) {
        io_ok = pullChunks();
    } else if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        if (url.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) {
            firmware_trace_stage_ = "fail";
            firmware_trace_result_ = "tls";
            ESP_LOGW("vibe_fw", "[fw] abort tls");
            fail("正在同步设备时间，以校验 HTTPS 证书");
            return;
        }
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.timeout_ms = 20000;
        config.buffer_size = 4096;
        if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
        auto *client = esp_http_client_init(&config);
        if (client && esp_http_client_open(client, 0) == ESP_OK &&
            esp_http_client_fetch_headers(client) >= 0 &&
            esp_http_client_get_status_code(client) == 200) {
            last_http = 200;
            io_ok = true;
            char buf[1024];
            int n = 0;
            while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                if (esp_ota_write(handle, buf, n) != ESP_OK) { io_ok = false; break; }
                if (hash_ok) mbedtls_md_update(&md, reinterpret_cast<const unsigned char *>(buf), n);
                written += static_cast<size_t>(n);
                if (written > 8 * 1024 * 1024) { io_ok = false; break; }
            }
            if (n < 0) io_ok = false;
        } else if (client) {
            last_http = esp_http_client_get_status_code(client);
        }
        firmware_trace_http_ = last_http;
        firmware_trace_written_ = static_cast<int>(written);
        ESP_LOGW("vibe_fw", "[fw] direct http=%d written=%u io=%d", last_http, static_cast<unsigned>(written), io_ok ? 1 : 0);
        if (client) esp_http_client_cleanup(client);
    } else {
        firmware_trace_stage_ = "fail";
        firmware_trace_result_ = "badaddr";
        ESP_LOGW("vibe_fw", "[fw] abort bad-url %.160s", url.c_str());
        fail("固件地址无效");
        return;
    }
    if (!io_ok || written < 16 || (size > 0 && written != static_cast<size_t>(size))) {
        firmware_trace_stage_ = "fail";
        firmware_trace_result_ = "incomplete";
        firmware_trace_written_ = static_cast<int>(written);
        firmware_trace_http_ = last_http;
        ESP_LOGW("vibe_fw", "[fw] incomplete io=%d written=%u size=%d http=%d tasks=%d task=%s",
            io_ok ? 1 : 0, static_cast<unsigned>(written), size, last_http,
            firmware_trace_tasks_, firmware_trace_task_.c_str());
        fail("固件下载不完整");
        return;
    }
    unsigned char digest[32] = {};
    if (!hash_ok || mbedtls_md_finish(&md, digest) != 0) {
        firmware_trace_stage_ = "fail";
        firmware_trace_result_ = "hash";
        ESP_LOGW("vibe_fw", "[fw] hash-setup written=%u", static_cast<unsigned>(written));
        fail("固件校验失败");
        return;
    }
    mbedtls_md_free(&md);
    char hex[65] = {};
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (sha.size() == 64 && sha != hex) {
        firmware_trace_stage_ = "fail";
        firmware_trace_result_ = "mismatch";
        ESP_LOGW("vibe_fw", "[fw] sha mismatch written=%u", static_cast<unsigned>(written));
        esp_ota_abort(handle);
        note(T("固件校验不一致"));
        return;
    }
    if (esp_ota_end(handle) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        firmware_trace_stage_ = "fail";
        firmware_trace_result_ = "boot";
        ESP_LOGW("vibe_fw", "[fw] boot-partition written=%u", static_cast<unsigned>(written));
        note(T("固件写入失败"));
        return;
    }
    if (sha.size() == 64) rememberFirmwareMark("sha", sha);
    rememberFirmwareMark("url", url);
    firmware_trace_stage_ = "done";
    firmware_trace_result_ = "ok";
    firmware_trace_written_ = static_cast<int>(written);
    ESP_LOGW("vibe_fw", "[fw] done written=%u size=%d", static_cast<unsigned>(written), size);
    note(version.empty() ? T("固件已更新，正在重启") : T("固件已更新，正在重启"));
    sendHeartbeat();
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

void VibeCoding::sendHeartbeat()
{
#if VIBE_SMOKE_TEST
    static uint32_t s_hb = 0;
    if ((s_hb++ % 10) == 0) ESP_LOGW("vibe_hb", "[hb] enter");
#endif
    std::string response;
    int status = 0;
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    // 截屏看门狗：上传任务若被中断（桥接器被杀 / USB 断开导致请求任务未走到复位分支）
    // 会把 screenshot_busy_ 永久卡在 true，使后续 debug.screenshot 全部静默 early-return。
    // 超过阈值（正常上传 ≤20s 超时）强制复位，自愈卡死。
    if (screenshot_busy_.load()) {
        const uint32_t age = now_ms - screenshot_busy_since_ms_.load();
        if (age > 25000) {
            ESP_LOGW("vibe_shot", "[screenshot] 看门狗复位卡死标志（已卡 %ums）", age);
            screenshot_busy_.store(false);
            screenshot_busy_since_ms_.store(0);
        }
    }
    const std::string path = "/device/heartbeat?uiStage=" + std::to_string(ui_stage_.load()) +
        "&uiAge=" + std::to_string(now_ms - ui_tick_ms_.load()) +
        "&bootAge=" + std::to_string(now_ms - boot_tick_ms_.load()) +
        "&exit=" + std::to_string(exit_requested_.load()) +
        "&displayStage=" + std::to_string(display_stage_.load()) +
        "&displayCount=" + std::to_string(display_count_.load()) +
        "&provider=" + std::to_string(selected_provider_.load()) +
        "&wifiReason=" + std::to_string(vibe_wifi::disconnect_reason()) +
        "&wifiDrops=" + std::to_string(vibe_wifi::disconnect_count()) +
        "&wifiSelf=" + std::to_string(vibe_wifi::voluntary_disconnects()) +
        "&wifiHist=" + vibe_wifi::disconnect_history() +
        "&heap=" + std::to_string(esp_get_free_heap_size()) +
        "&minHeap=" + std::to_string(esp_get_minimum_free_heap_size()) +
        "&psram=" + std::to_string(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) +
        "&psramMin=" + std::to_string(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)) +
        "&upMs=" + std::to_string(now_ms) +
        "&fwStage=" + firmware_trace_stage_ +
        "&fwWhy=" + firmware_trace_why_ +
        "&fwResult=" + firmware_trace_result_ +
        "&fwWritten=" + std::to_string(firmware_trace_written_) +
        "&fwSize=" + std::to_string(firmware_trace_size_) +
        "&fwHttp=" + std::to_string(firmware_trace_http_) +
        "&fwTasks=" + std::to_string(firmware_trace_tasks_) +
        "&fwTask=" + firmware_trace_task_;
#if VIBE_SMOKE_TEST
    // 滑动链路遥测（观察期专用）：console 抓取不可靠，心跳 URL 是唯一稳定
    // 通道。burnin=到点次数 swipe=handleUiSwipe 进入 choose=通过 voice 守卫
    // cmd=收到的远程命令 drawer/set=卡死排查 voice=语音状态机 phase 值。
    const auto voice_now = vibe_voice::status();
    const std::string burnin_query = std::string("&burnin=") + std::to_string(burnin_ticks_.load()) +
        "&swipe=" + std::to_string(swipe_calls_.load()) +
        "&choose=" + std::to_string(choose_calls_.load()) +
        "&cmd=" + std::to_string(cmd_calls_.load()) +
        "&drawer=" + std::to_string(drawer_open_.load()) +
        "&set=" + std::to_string(settings_open_.load()) +
        "&voice=" + std::to_string(static_cast<int>(voice_now.phase)) +
        "&shotBusy=" + std::to_string(screenshot_busy_.load());
    const std::string path_with_query = path + burnin_query;
#endif
#if VIBE_SMOKE_TEST
    const bool sent = request(path_with_query, false, response, status);
#else
    const bool sent = request(path, false, response, status);
#endif
    if (receiverVoiceUploading()) return;
    if (!sent) {
        setConnectionError("Computer bridge offline");
        return;
    }
    if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        setConnectionError("Computer unpaired this device");
        return;
    }
    if (status != 200) {
        setConnectionError("Bridge heartbeat HTTP " + std::to_string(status));
        return;
    }
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (model_.connection_error == "Computer bridge offline" ||
        model_.connection_error.rfind("Bridge heartbeat HTTP ", 0) == 0) {
        model_.connection_error.clear();
        ++model_.revision;
    }
}

void VibeCoding::refreshSessions()
{
    std::string provider_id;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        provider_id = selectedProviderLocked().id;
    }
    if (!vibe_provider::validId(provider_id)) return;
    std::string response;
    int status = 0;
    const bool sent = request(std::string("/device/sessions?provider=") + provider_id,
                              false, response, status);
    if (receiverVoiceUploading()) return;
    if (!sent) {
        setConnectionError("Bridge unreachable");
        return;
    }
    if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        setConnectionError("Computer unpaired this device");
        return;
    }
    if (status != 200) {
        setConnectionError("Bridge sessions HTTP " + std::to_string(status));
        return;
    }
    cJSON *root = cJSON_Parse(response.c_str());
    const cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "sessions") : nullptr;
    if (!cJSON_IsArray(items)) {
        if (root) cJSON_Delete(root);
        setConnectionError("Invalid session list");
        return;
    }
    std::vector<Session> sessions;
    const int count = std::min(cJSON_GetArraySize(items), 32);
    sessions.reserve(count);
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) continue;
        Session session = {
            jsonString(item, "id"),
            jsonString(item, "title"),
            jsonString(item, "provider"),
            jsonString(item, "projectId"),
        };
        if (validSessionId(session.id) && session.provider == provider_id &&
            validProjectId(session.project_id)) {
            sessions.push_back(std::move(session));
        }
    }
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (selectedProviderLocked().id != provider_id) return;
        auto *provider = findProviderLocked(provider_id);
        if (!provider) return;
        auto &selected_id = provider->selected_session_id;
        const bool selected_exists = std::any_of(sessions.begin(), sessions.end(),
            [&selected_id](const Session &session) { return session.id == selected_id; });
        const std::string next_id = selected_exists ? selected_id :
            (sessions.empty() ? "" : sessions.front().id);
        if (selected_id != next_id) {
            selected_id = next_id;
            model_.tasks.clear();
            model_.feedback.clear();
            model_.feedback_task_id.clear();
            visible_task_id_.clear();
        }
        model_.sessions = std::move(sessions);
        model_.connection_error.clear();
        ++model_.revision;
    }
}

void VibeCoding::createSession(const std::string &provider_id, const std::string &project_id)
{
    if (!vibe_provider::validId(provider_id) || !validProjectId(project_id)) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        create_session_in_flight_ = false;
        model_.session_notice = T("新建任务参数无效");
        ++model_.revision;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto *provider = findProviderLocked(provider_id);
        if (!provider || !provider->available) {
            create_session_in_flight_ = false;
            model_.session_notice = T("此助手已移除或不可用");
            ++model_.revision;
            return;
        }
    }
    std::string response;
    int status = 0;
    const std::string path = std::string("/device/sessions?provider=") +
        provider_id + "&projectId=" + project_id;
    const bool sent = request(path, true, response, status);
    std::string notice;
    Session created;
    if (sent && (status == 200 || status == 201)) {
        cJSON *root = cJSON_Parse(response.c_str());
        if (root) {
            created = {jsonString(root, "id"), jsonString(root, "title"),
                       jsonString(root, "provider"), jsonString(root, "projectId")};
            cJSON_Delete(root);
        }
        if (!validSessionId(created.id) || created.provider != provider_id ||
            created.project_id != project_id) {
            created = {};
            notice = T("新建任务响应无效");
        }
    } else if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        notice = T("电脑已解除配对");
    } else {
        notice = sent ? T("新建任务失败 (HTTP ") + std::to_string(status) + ")" : T("电脑连接失败");
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        create_session_in_flight_ = false;
        auto *provider = findProviderLocked(provider_id);
        if (!created.id.empty() && provider) {
            provider->selected_session_id = created.id;
            if (selectedProviderLocked().id == provider_id) {
                model_.sessions.insert(model_.sessions.begin(), std::move(created));
                model_.tasks.clear();
                model_.feedback.clear();
                model_.feedback_task_id.clear();
                visible_task_id_.clear();
                model_.session_notice = provider->external_unscoped ?
                    T("此助手任务上下文可能共享，请在电脑端核对") :
                    T("任务已创建，按住 BOOT 说话");
            }
        } else if (selectedProviderLocked().id == provider_id) {
            model_.session_notice = std::move(notice);
        }
        ++model_.revision;
    }
}

void VibeCoding::deleteSession(const std::string &provider_id, const std::string &session_id)
{
    std::string response;
    int status = 0;
    const bool sent = validSessionId(session_id) && vibe_provider::validId(provider_id) &&
        request(std::string("/device/sessions/") + session_id + "/delete", true, response, status);
    bool deleted = false;
    std::string notice;
    cJSON *root = sent ? cJSON_Parse(response.c_str()) : nullptr;
    if (sent && status == 200 && root && jsonString(root, "id") == session_id &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "deleted"))) {
        deleted = true;
        notice = T("任务已删除");
    } else if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        notice = T("电脑已解除配对");
    } else if (!sent) {
        notice = T("电脑连接失败");
    } else if (root && !jsonString(root, "error").empty()) {
        notice = jsonString(root, "error");
    } else {
        notice = T("删除任务失败 (HTTP ") + std::to_string(status) + ")";
    }
    if (root) cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        delete_session_in_flight_ = false;
        auto *provider = findProviderLocked(provider_id);
        if (deleted && provider && provider->selected_session_id == session_id)
            provider->selected_session_id.clear();
        if (selectedProviderLocked().id == provider_id) {
            if (deleted) {
                model_.sessions.erase(std::remove_if(model_.sessions.begin(), model_.sessions.end(),
                    [&session_id](const Session &item) { return item.id == session_id; }), model_.sessions.end());
                model_.tasks.clear();
                model_.feedback.clear();
                model_.feedback_task_id.clear();
                visible_task_id_.clear();
                voice_session_id_.clear();
            }
            model_.session_notice = std::move(notice);
            ++model_.revision;
        }
    }
}

void VibeCoding::refreshTasks()
{
    std::string provider_id;
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto &provider = selectedProviderLocked();
        provider_id = provider.id;
        session_id = provider.selected_session_id;
    }
    if (!validSessionId(session_id)) return;
    std::string response;
    int status = 0;
    const bool sent = request(std::string("/device/tasks?provider=") + provider_id +
                              "&sessionId=" + session_id, false, response, status);
    if (receiverVoiceUploading()) return;
    if (!sent) {
        setConnectionError("Bridge unreachable");
        return;
    }
    if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        setConnectionError("Computer unpaired this device");
        return;
    }
    if (status != 200) {
        setConnectionError("Bridge HTTP " + std::to_string(status));
        return;
    }
    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) {
        setConnectionError("Invalid bridge response");
        return;
    }
    const cJSON *jobs = cJSON_GetObjectItemCaseSensitive(root, "jobs");
    if (!cJSON_IsArray(jobs)) {
        cJSON_Delete(root);
        setConnectionError("Invalid task list");
        return;
    }
    std::vector<Task> tasks;
    const int count = std::min(cJSON_GetArraySize(jobs), 8);
    tasks.reserve(1);
    for (int i = 0; i < count; ++i) {
        const cJSON *job = cJSON_GetArrayItem(jobs, i);
        if (!cJSON_IsObject(job)) continue;
        Task task = {
            jsonString(job, "id"),
            jsonString(job, "instruction"),
            jsonString(job, "provider"),
            jsonString(job, "projectId"),
            jsonString(job, "status"),
            jsonString(job, "result"),
            jsonString(job, "progress"),
            jsonString(job, "error"),
            jsonString(job, "vibeSessionId"),
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "resultTruncated")) != 0,
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "instructionTruncated")) != 0,
            parseAttachments(job),
        };
        if (validTaskId(task.id) && task.provider == provider_id &&
            task.session_id == session_id) {
            tasks.push_back(std::move(task));
            break; // The current task shows its newest exchange only.
        }
    }
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (selectedProviderLocked().id != provider_id ||
            selectedProviderLocked().selected_session_id != session_id) return;
        model_.tasks = std::move(tasks);
        model_.connection_error.clear();
        model_.feedback_external = false;
        if (model_.feedback == "Confirmed" || model_.feedback == "Cancelled" ||
            std::none_of(model_.tasks.begin(), model_.tasks.end(), [this](const Task &task) {
                return task.id == model_.feedback_task_id &&
                    (task.status == "waiting_confirmation" || task.status == "queued" || task.status == "running");
            })) {
            model_.feedback.clear();
            model_.feedback_task_id.clear();
        }
        ++model_.revision;
    }
}

void VibeCoding::sendAction(PendingAction action)
{
    std::string response;
    int status = 0;
    const bool sent = request("/device/tasks/" + action.id + "/" + action.verb, true, response, status);
    std::string feedback;
    bool external_feedback = false;
    if (!sent) {
        feedback = "Action could not reach bridge";
    } else if (status == 200) {
        feedback = action.verb == "confirm" ? "Confirmed" : "Cancelled";
    } else {
        cJSON *root = cJSON_Parse(response.c_str());
        feedback = root ? jsonString(root, "error") : "";
        external_feedback = !feedback.empty();
        if (root) cJSON_Delete(root);
        if (feedback.empty()) feedback = "Action rejected (HTTP " + std::to_string(status) + ")";
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        model_.feedback_external = external_feedback;
        model_.feedback = std::move(feedback);
        model_.feedback_task_id = action.id;
        model_.action_in_flight = false;
        ++model_.revision;
    }
}

void VibeCoding::workerEntry(void *arg)
{
    static_cast<VibeCoding *>(arg)->workerLoop();
}

void VibeCoding::ensureWorkerStarted()
{
    // 摇晃感知（vibe_motion）：设备有 IMU 就启动，检测到「摇一摇」回调进
    // notifyMotionShake（内部带幂等，无 IMU 静默不启动）。
    vibe_motion::start([](void *ctx) { static_cast<VibeCoding *>(ctx)->notifyMotionShake(); }, this);
    bool expected = false;
    if (worker_started_.compare_exchange_strong(expected, true) &&
        xTaskCreate(workerEntry, "vibe_bridge", 12288, this, 3, nullptr) != pdPASS) {
        worker_started_ = false;
        setConnectionError("Cannot start network task");
    }
}

void VibeCoding::workerLoop()
{
    uint32_t active_cycles = 0;
    uint64_t last_heartbeat_ms = 0;
    uint32_t seen_app_epoch = button_epoch_.load();
    bool phone_setup_started = false;
    uint32_t phone_epoch = 0;
    while (true) {
        const uint32_t app_epoch = button_epoch_.load();
        if (app_epoch != seen_app_epoch) {
            seen_app_epoch = app_epoch;
            last_heartbeat_ms = 0;
        }
        bool has_pending_action = false;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            has_pending_action = !pending_action_.verb.empty();
        }
        const bool foreground = canOperate();
#if VIBE_SMOKE_TEST
        static uint32_t s_wdiag = 0;
        if ((s_wdiag++ % 200) == 0) {
            const auto ps = vibe_pairing::snapshot();
            ESP_LOGW("vibe_wk", "[wk] fg=%d active=%d exit=%d phone=%d cfg=%d phase=%d url=%d token=%d",
                     foreground, active_.load(), exit_requested_.load(),
                     phone_setup_started, config_loaded_.load(),
                     static_cast<int>(ps.phase), !ps.url.empty(), !ps.token.empty());
        }
#endif
        const uint32_t requested_phone_epoch = phone_setup_epoch_.load();
        const bool want_phone = foreground && !exit_requested_.load() && phone_setup_requested_.load();
        // The portal must run before the Wi-Fi readiness gates, including when
        // the device has never joined a network. All blocking cleanup stays here.
        if ((phone_setup_started || vibe_wifi::setup_ap_active()) && (!want_phone || requested_phone_epoch != phone_epoch)) {
            vibe_phone_setup::stop();
            phone_setup_started = false;
            if (vibe_wifi::setup_ap_active()) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }
        if (restore_station_pending_.load() && !active_.load() &&
            (!vibe_wifi::receiver_active() || vibe_wifi::restore_station() == ESP_OK))
            restore_station_pending_.store(false);
        if (want_phone) {
            auto fail_phone = [this, requested_phone_epoch](const std::string &message) {
                if (requested_phone_epoch != phone_setup_epoch_.load() ||
                    !phone_setup_requested_.load() || !active_.load()) return;
                { std::lock_guard<std::mutex> lock(model_mutex_); phone_setup_error_ = message; }
                phone_setup_requested_.store(false);
                phone_setup_result_.store(-1);
            };
            if (!phone_setup_started) {
                phone_epoch = requested_phone_epoch;
                vibe_phone_setup::InitialConfig initial;
                const auto pairing = vibe_pairing::snapshot();
                initial.mode = pairing.access_mode == vibe_pairing::AccessMode::Manual ? "manual" :
                               pairing.access_mode == vibe_pairing::AccessMode::Receiver ? "receiver" : "automatic";
                char receiver_ssid[33] = {};
                if (vibe_wifi::receiver_ssid(receiver_ssid, sizeof(receiver_ssid))) initial.receiver_ssid = receiver_ssid;
                initial.https = pairing.manual_url.empty() || pairing.manual_url.rfind("https://", 0) == 0;
                initial.port = initial.https ? 443 : 8788;
                if (!pairing.manual_url.empty()) {
                    const size_t scheme = pairing.manual_url.find("://");
                    const std::string authority = scheme == std::string::npos ? pairing.manual_url : pairing.manual_url.substr(scheme + 3);
                    const size_t colon = authority.rfind(':');
                    initial.host = colon == std::string::npos ? authority : authority.substr(0, colon);
                    if (colon != std::string::npos) initial.port = static_cast<uint16_t>(std::strtoul(authority.substr(colon + 1).c_str(), nullptr, 10));
                }
                const esp_err_t result = vibe_phone_setup::start(vibe_i18n::locale() == vibe_i18n::Locale::English, initial);
                phone_setup_started = true; // Also clean up a partially started portal.
                phone_setup_ready_epoch_.store(phone_epoch);
                if (result != ESP_OK) fail_phone(T("手机配置开启失败，请返回重试"));
            }
            // Back / long BOOT / app close may have happened during startup.
            if (!active_.load() || exit_requested_.load() || !phone_setup_requested_.load() ||
                phone_epoch != phone_setup_epoch_.load()) {
                vibe_phone_setup::stop();
                phone_setup_started = false;
                if (!active_.load() && vibe_wifi::receiver_active()) (void)vibe_wifi::restore_station();
                continue;
            }
            vibe_phone_setup::tick();
            const auto status = vibe_phone_setup::snapshot();
            if (status.phase == vibe_phone_setup::Phase::Expired || status.phase == vibe_phone_setup::Phase::Error) {
                vibe_phone_setup::stop();
                phone_setup_started = false;
                fail_phone(status.phase == vibe_phone_setup::Phase::Expired ? T("手机配置已超时，请重新开启") : T("手机配置开启失败，请返回重试"));
                continue;
            }
            vibe_phone_setup::Submission submission;
            if (vibe_phone_setup::take_submission(submission)) {
                vibe_phone_setup::stop();
                phone_setup_started = false;
                if (active_.load() && !exit_requested_.load() && phone_setup_requested_.load() &&
                    phone_epoch == phone_setup_epoch_.load()) {
                    std::string error;
                    bool saved = false;
                    if (vibe_wifi::setup_ap_active()) error = T("手机配置关闭失败，请返回重试");
                    else saved = applyPhoneSetup(submission, error);
                    if (saved) {
                        if (phone_epoch == phone_setup_epoch_.load() && phone_setup_requested_.load()) {
                            phone_setup_requested_.store(false);
                            phone_setup_result_.store(1);
                        }
                    } else fail_phone(error.empty() ? T("手机配置保存失败，请重试") : vibe_i18n::message(error));
                }
                std::fill(submission.wifi_password.begin(), submission.wifi_password.end(), '\0');
                std::fill(submission.receiver_password.begin(), submission.receiver_password.end(), '\0');
                last_heartbeat_ms = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!foreground && !has_pending_action) {
            last_heartbeat_ms = 0; // Re-entering Vibe sends the first heartbeat immediately.
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // Apply saved mode changes even when the previous Wi-Fi is offline.
        if (foreground) vibe_pairing::apply_pending_access();
        // Entry/exit station switching shares this worker with phone setup.
        // A rapid reopen cannot race an old exit into the wrong Wi-Fi mode.
        if (canOperate() && foreground_network_pending_.exchange(false)) {
            // A network chosen in 网络设置 stays up. Receiver mode reconnects
            // only when that choice is still the active station.
            if (vibe_pairing::snapshot().access_mode == vibe_pairing::AccessMode::Receiver &&
                !vibe_wifi::station_held() && !vibe_wifi::receiver_active() &&
                vibe_wifi::connect_receiver() != ESP_OK)
                setConnectionError(T("请在接入设置填写接收端热点信息"));
        }
        const bool usb_direct_active = canOperate() &&
#if VIBE_SMOKE_TEST
            true;  // 冒烟测试：强制 USB direct，无视配对模式
#else
            vibe_pairing::snapshot().access_mode == vibe_pairing::AccessMode::UsbDirect;
#endif
        if (vibe_usb::active() != usb_direct_active) vibe_usb::set_active(usb_direct_active);
        if (!canOperate()) vibe_usb::set_active(false);
        // Both USB transports handle one request exchange at a time.
        // Transcription can take minutes; let the voice task own the link.
        if (foreground && receiverVoiceUploading()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        auto failPendingAction = [this](const char *reason) {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (!pending_action_.verb.empty()) {
                pending_action_ = {};
                model_.action_in_flight = false;
                model_.feedback_external = false;
                model_.feedback = reason;
                ++model_.revision;
            }
            if (pending_create_session_) {
                pending_create_session_ = false;
                pending_create_provider_id_.clear();
                pending_create_project_id_.clear();
                model_.session_notice = reason;
                ++model_.revision;
            }
        };
        const bool usb_direct =
#if VIBE_SMOKE_TEST
            true;  // 冒烟测试：应用层请求也强制走 USB direct（无视配对模式）
#else
            vibe_pairing::snapshot().access_mode == vibe_pairing::AccessMode::UsbDirect;
#endif
        wifi_ap_record_t ap = {};
        if (!usb_direct && (!vibe_wifi::is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK)) {
            const bool receiver = vibe_pairing::snapshot().access_mode == vibe_pairing::AccessMode::Receiver ||
                vibe_wifi::receiver_active();
            if (receiver && vibe_pairing::snapshot().phase == vibe_pairing::Phase::Paired)
                vibe_pairing::begin_foreground_connection();
            invalidateCatalog();
            setConnectionError(receiver ? "Connect receiver hotspot first" : "Connect Wi-Fi first");
            failPendingAction(receiver ? "Action not sent: receiver offline" :
                              "Action not sent: Wi-Fi offline");
            vTaskDelay(pdMS_TO_TICKS(foreground ? POLL_INTERVAL_MS : 500));
            continue;
        }
        if (foreground) vibe_pairing::tick();
        const auto access = vibe_pairing::snapshot();
#if !VIBE_SMOKE_TEST
        if (access.phase == vibe_pairing::Phase::Paired &&
            access.url.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) {
            setConnectionError(vibe_pairing::tls_time_failed() ?
                T("设备时间同步失败，无法校验 HTTPS 证书") : T("正在同步设备时间，以校验 HTTPS 证书"));
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }
#endif
        const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
#if VIBE_SMOKE_TEST
        if (canOperate() &&
            (last_heartbeat_ms == 0 || now_ms - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS)) {
#else
        if (canOperate() && vibe_pairing::authorized_for_foreground() &&
            vibe_pairing::snapshot().phase == vibe_pairing::Phase::Paired &&
            (last_heartbeat_ms == 0 || now_ms - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS)) {
#endif
            last_heartbeat_ms = now_ms;
            sendHeartbeat();
        }
#if !VIBE_SMOKE_TEST
        const auto pairing = vibe_pairing::snapshot();
        if (pairing.phase != vibe_pairing::Phase::Paired ||
            !vibe_pairing::authorized_for_foreground()) {
            setConnectionError(pairing.message);
            failPendingAction("Action not sent: computer not paired");
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }
#endif
        if (!active_.load()) {
            // Finish an already requested confirmation/cancellation, then go idle.
            PendingAction action;
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                action = std::move(pending_action_);
                pending_action_ = {};
            }
            if (!action.verb.empty()) sendAction(std::move(action));
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (canOperate() &&
            (language_refresh_pending_.exchange(false) || (active_cycles++ % 14) == 0 || !config_loaded_.load() || !voice_available_.load())) {
            refreshConfig();
        }
        if (canOperate() &&
            (artwork_request_.exchange(false) || artworkRetryDue())) {
            // 内存紧张兜底：syncArtwork 内部分配失败不得击穿 worker 线程
            // （异常逃出会 std::terminate → abort → 反复重启）。
            try {
                syncArtwork();
            } catch (...) {
                ESP_LOGE(BUTTON_TAG, "[sprite] sync_artwork_exception");
            }
        }
        if (artwork_purge_pending_.exchange(false)) {
            // LVGL 判定缓存损坏（解码连败）：整套删掉，标记 dirty 让
            // syncArtwork/preload 走 ensure() 完整重下（ensure 逐文件验哈希）。
            std::string purge_provider;
            {
                std::lock_guard<std::mutex> lock(artwork_mutex_);
                purge_provider = std::move(artwork_purge_provider_);
                artwork_purge_provider_.clear();
            }
            if (!purge_provider.empty()) {
                made_sprite_cache::purge(purge_provider);
                artwork_dirty_.store(true);
                artwork_request_.store(true);
            }
        }
        if (firmware_update_requested_.exchange(false)) runFirmwareUpdate();
        preloadArtworkCache();
        PendingAction action;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            action = std::move(pending_action_);
            pending_action_ = {};
        }
        if (!action.verb.empty()) sendAction(std::move(action));
        std::string create_provider;
        std::string create_project;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (pending_create_session_ && !create_session_in_flight_) {
                create_provider = std::move(pending_create_provider_id_);
                create_project = std::move(pending_create_project_id_);
                pending_create_session_ = false;
                pending_create_provider_id_.clear();
                create_session_in_flight_ = true;
            }
        }
        if (!create_provider.empty()) createSession(create_provider, create_project);
        std::string delete_provider;
        std::string delete_session_id;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (pending_delete_session_ && !delete_session_in_flight_) {
                delete_provider = std::move(pending_delete_provider_id_);
                delete_session_id = std::move(pending_delete_session_id_);
                pending_delete_session_ = false;
                pending_delete_provider_id_.clear();
                pending_delete_session_id_.clear();
                delete_session_in_flight_ = true;
            }
        }
        if (!delete_session_id.empty()) deleteSession(delete_provider, delete_session_id);
        if (canOperate() && config_loaded_.load()) {
            if (!board_catalog_sent_) announceBoardCatalog();
            else {
                // 动作名单变了（素材切换等）：整张清单带新 action 条目重报。
                std::vector<std::string> current;
                {
                    std::lock_guard<std::mutex> lock(model_mutex_);
                    current = available_actions_;
                }
                if (current != announced_actions_) announceBoardCatalog();
            }
            flushBoardEvent();
            pullBoardCommands();
            refreshSessions();
            refreshTasks();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

} // namespace esp_brookesia::apps
