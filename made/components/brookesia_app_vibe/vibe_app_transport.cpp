// SPDX-License-Identifier: Apache-2.0
// 传输模块：设备↔桥接器/云端的 HTTP 通道（USB direct / Receiver / HTTPS
// 三分支统一入口）与 /device/config 拉取解析。
// 配对门禁、冒烟伪造凭据、旧通道残 reply 丢弃都在 requestRaw 一处收口。
#include "vibe_smoke_test.h"
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_ui_shared.hpp"

#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "vibe_i18n.hpp"
#include "vibe_pairing.hpp"
#include "vibe_usb.hpp"

namespace esp_brookesia::apps {

namespace {

// esp_http_client 事件捕获：把响应体追加进 std::string（带大小上限）。
struct HttpCapture {
    std::string *body = nullptr;
    size_t limit = 0;
};

} // namespace

esp_err_t VibeCoding::httpEvent(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || event->data_len <= 0) return ESP_OK;
    auto *capture = static_cast<HttpCapture *>(event->user_data);
    if (!capture->body || capture->body->size() + static_cast<size_t>(event->data_len) > capture->limit) return ESP_FAIL;
    capture->body->append(static_cast<const char *>(event->data), event->data_len);
    return ESP_OK;
}

bool VibeCoding::request(const std::string &path, bool post, std::string &response, int &status,
                         const std::string &json_body, size_t response_limit, int timeout_ms)
{
    return requestRaw(path, post, json_body.empty() ? nullptr : "application/json",
                      json_body.empty() ? nullptr
                                        : reinterpret_cast<const uint8_t *>(json_body.data()),
                      json_body.size(), response, status, response_limit, timeout_ms);
}

bool VibeCoding::requestRaw(const std::string &path, bool post, const char *content_type,
                            const uint8_t *body, size_t body_len, std::string &response, int &status,
                            size_t response_limit, int timeout_ms)
{
    if (!post && receiverVoiceUploading()) return false;
    const auto pairing = vibe_pairing::snapshot();
#if VIBE_SMOKE_TEST
    // 冒烟测试：设备侧配对记录可能为空，伪造凭据（USB direct 通道桥接器
    // 侧物理信任，token 不参与校验）。
    std::string base = pairing.url.empty() ? "http://burnin.local" : pairing.url;
    const std::string token = pairing.token.empty() ? "baeeabde0ae90cbff6e590641c6d91827c537228d7e56f862fc5244cc7596f05" : pairing.token;
#else
    std::string base = pairing.url;
    const std::string token = pairing.token;
#endif
    if (base.empty() || token.empty() || !vibe_pairing::authorized_for_foreground()) {
#if VIBE_SMOKE_TEST
        ESP_LOGW("vibe_req", "[req-gate] base=%d token=%d auth=%d", !base.empty(), !token.empty(),
                 vibe_pairing::authorized_for_foreground());
#endif
        return false;
    }
#if !VIBE_SMOKE_TEST
    if (base.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) {
        setConnectionError(vibe_pairing::tls_time_failed() ?
            T("设备时间同步失败，无法校验 HTTPS 证书") : T("正在同步设备时间，以校验 HTTPS 证书"));
        return false;
    }
#endif
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string wire_path = vibe_i18n::request_path(path);
    const std::string url = base + wire_path;
    const std::string authorization = "Bearer " + token;
    esp_err_t error = ESP_FAIL;
    if (VIBE_SMOKE_TEST ||
        pairing.access_mode == vibe_pairing::AccessMode::UsbDirect) {
        error = vibe_usb::request(wire_path, post, authorization,
                                  content_type ? content_type : "",
                                  body, body_len,
                                  response, status, response_limit, timeout_ms);
#if VIBE_SMOKE_TEST
        if (error != ESP_OK)
            ESP_LOGW("vibe_req", "[req-usb] %s err=%d", wire_path.c_str(), error);
#endif
    } else if (pairing.access_mode == vibe_pairing::AccessMode::Receiver) {
        error = vibe_usb::receiver_request(wire_path, post, authorization,
                                            content_type ? content_type : "",
                                            body, body_len,
                                            response, status, response_limit, timeout_ms);
    } else {
        HttpCapture capture{&response, response_limit};
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.disable_auto_redirect = true;
        if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
        config.event_handler = httpEvent;
        config.user_data = &capture;
        config.timeout_ms = url.rfind("https://", 0) == 0 && timeout_ms < REMOTE_REQUEST_TIMEOUT_MS
                                ? REMOTE_REQUEST_TIMEOUT_MS : timeout_ms;
        config.buffer_size = 1024;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return false;
        esp_http_client_set_header(client, "ngrok-skip-browser-warning", "1");
        esp_http_client_set_header(client, "Authorization", authorization.c_str());
        if (post) esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (post && body && body_len > 0) {
            esp_http_client_set_header(client, "Content-Type", content_type ? content_type : "application/json");
            esp_http_client_set_post_field(client, reinterpret_cast<const char *>(body),
                                           static_cast<int>(body_len));
        }
        error = esp_http_client_perform(client);
        status = error == ESP_OK ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);
    }
    const auto latest = vibe_pairing::snapshot();
#if VIBE_SMOKE_TEST
    // 压测伪造 token 与真实配对快照（token 为空）必然不等，上面的检查会把
    // 每一个成功响应都当「旧通道残 reply」丢弃——这正是 config 无限重拉、
    // providers 永不填充的根因。USB direct 物理信任，压测期直接放行。
#else
    if (!vibe_pairing::authorized_for_foreground() || latest.url != pairing.url ||
        latest.token != token) {
        status = 0;
        return false; // Discard a reply from the previous access mode.
    }
#endif
    if (error != ESP_OK) {
        // 单次传输失败不清目录：USB cable 抖动 / WiFi 瞬断（diag=e=cable
        // 实锤每 10-40s 一次）此前会 invalidateCatalog → UI 闪「连接电脑」
        // 数秒。目录刷新交给周期 config 轮询（~31s 内）；真断线时 UI 短暂
        // 冻结在旧目录，好过反复闪屏。拔线等真失效由 pairing 层处理。
        vibe_pairing::reconnect();
    }
    return error == ESP_OK;
}

namespace {

// 固件地址/SHA 合法性与「已装当前版本」判定（refreshConfig 专用纯函数，
// 输入输出全是值类型，可脱离设备单独单元测试）。
struct FirmwareOffer {
    std::string url;
    std::string sha;
    std::string version;
    int size = 0;
    const char *why = "empty";   // 决策依据（遥测用）
    bool already_installed = false;
};

FirmwareOffer evaluateFirmwareOffer(std::string url, std::string sha, const std::string &version, int size,
                                    const std::string &saved_sha, const std::string &saved_url)
{
    FirmwareOffer offer;
    if (url.size() > 180) url.resize(180);
    const bool sha_ok = sha.size() == 64 &&
        sha.find_first_not_of("0123456789abcdef") == std::string::npos;
    if (!sha_ok) sha.clear();
    const bool sha_hit = sha_ok && saved_sha == sha;
    const bool url_hit = !sha_ok && saved_url == url;
    offer.url = std::move(url);
    offer.sha = std::move(sha);
    offer.version = version;
    offer.size = size;
    if (offer.url.empty()) offer.why = "empty";
    else if (sha_ok && saved_sha.empty()) offer.why = "nomark";
    else if (sha_ok) offer.why = "sha";
    else if (saved_url.empty()) offer.why = "nomark";
    else offer.why = "url";
    offer.already_installed = !offer.url.empty() && (sha_hit || url_hit);
    return offer;
}

} // namespace

// config 响应里的固件信息 + 本地已装标记 → 待更新状态与文案（须持 model_mutex_）。
void VibeCoding::applyFirmwareOffer(const FirmwareFields &fields)
{
    const std::string saved_sha = savedFirmwareMark("sha");
    const std::string saved_url = savedFirmwareMark("url");
    const FirmwareOffer offer = evaluateFirmwareOffer(
        fields.url, fields.sha, fields.version, fields.size, saved_sha, saved_url);
    std::string task_status = "-";
    if (!model_.tasks.empty()) {
        task_status = model_.tasks.front().status;
        if (task_status.empty() || task_status.size() > 32 ||
            task_status.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
            task_status = "other";
        }
    }
    firmware_trace_why_ = offer.why;
    firmware_trace_task_ = task_status;
    firmware_trace_tasks_ = static_cast<int>(model_.tasks.size());
    firmware_trace_size_ = offer.size;
    // 配置只记下地址。下载要等设置页里点「下载更新」。
    if (!firmware_update_requested_.load()) {
        firmware_url_ = offer.url;
        firmware_sha_ = offer.sha;
        firmware_version_ = offer.version;
        firmware_size_ = offer.size;
    }
    const bool pending = !firmware_url_.empty() && !offer.already_installed;
    firmware_available_.store(pending);
    if (!firmware_update_requested_.load() && !(firmware_trace_stage_ == "fail" && pending)) {
        firmware_trace_stage_ = firmware_url_.empty() ? "idle" : (pending ? "ready" : "skip");
        firmware_trace_result_ = firmware_url_.empty() ? "none" : (pending ? "wait" : "match");
        firmware_trace_written_ = -1;
        firmware_trace_http_ = 0;
        if (firmware_url_.empty()) firmware_status_ = T("电脑上还没有配置固件地址");
        else if (!pending) firmware_status_ = T("已是当前固件");
        else if (firmware_version_.empty()) firmware_status_ = T("有可下载的固件");
        else {
            std::string shown = firmware_version_;
            if (shown.size() > 32) shown.resize(32);
            firmware_status_ = std::string(T("有可下载的固件")) + " " + shown;
        }
        const std::string mark = firmware_trace_stage_ + offer.why + ":" + std::to_string(firmware_size_);
        static std::string logged_mark;
        if (mark != logged_mark) {
            logged_mark = mark;
            ESP_LOGI("vibe_fw", "[fw] %s why=%s tasks=%d task=%s size=%d savedSha=%.12s offerSha=%.12s url=%.160s",
                firmware_trace_stage_.c_str(), offer.why, firmware_trace_tasks_, task_status.c_str(),
                firmware_size_, saved_sha.c_str(), firmware_sha_.c_str(), firmware_url_.c_str());
        }
    }
}

void VibeCoding::refreshConfig()
{
    const uint32_t epoch = catalog_epoch_.load();
    const auto origin = vibe_pairing::snapshot();
    std::string response;
    int status = 0;
    // config 响应含完整 provider/素材目录（实测 ~9KB），默认 8192 会被
    // receive_response 截断导致 parse 永远失败、config 无限重拉。
    if (!request("/device/config", false, response, status, "", 256 * 1024)) return;
#if VIBE_SMOKE_TEST
    ESP_LOGW("vibe_cfg", "[cfg] status=%d bytes=%u", status, static_cast<unsigned>(response.size()));
#endif
    if (receiverVoiceUploading()) return;
    if (status == 401) {
        vibe_pairing::forget();
        invalidateCatalog();
        return;
    }
    if (status != 200) { invalidateCatalog(); return; }
    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) { invalidateCatalog(); return; }

    std::vector<vibe_provider::Provider> providers;
    const bool has_catalog = vibe_provider::parse(cJSON_GetObjectItemCaseSensitive(root, "providers"), providers);
    const cJSON *voice = cJSON_GetObjectItemCaseSensitive(root, "voice");
    const bool can_voice = cJSON_IsObject(voice) &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(voice, "available"));
    const std::string project = jsonString(root, "defaultProject");
    const std::string received_locale = jsonString(root, "locale");
    std::vector<std::string> board_events;
    const cJSON *board = cJSON_GetObjectItemCaseSensitive(root, "board");
    const cJSON *event_list = cJSON_IsObject(board) ? cJSON_GetObjectItemCaseSensitive(board, "events") : nullptr;
    if (cJSON_IsArray(event_list)) {
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, event_list) {
            if (!cJSON_IsString(item) || !item->valuestring || board_events.size() >= 32) continue;
            const std::string name = item->valuestring;
            if (name.size() <= 48 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._") == std::string::npos)
                board_events.push_back(name);
        }
    }
    // announce 必须在 cJSON_Delete(root) 之前取值：删除后再读 root 是释放后使用，
    // 实测会把「桥接器重启后请重报清单」的指令整个丢掉（只有开机路径才重报）。
    const bool announce_catalog = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "announce"));
    FirmwareFields firmware{
        jsonString(root, "firmwareUrl"),
        jsonString(root, "firmwareSha256"),
        jsonString(root, "firmwareVersion"),
        0};
    const cJSON *firmware_size_item = cJSON_GetObjectItemCaseSensitive(root, "firmwareSize");
    firmware.size = cJSON_IsNumber(firmware_size_item) && firmware_size_item->valuedouble > 0 &&
        firmware_size_item->valuedouble <= 8 * 1024 * 1024
        ? static_cast<int>(firmware_size_item->valuedouble) : 0;
    cJSON_Delete(root);
    if (!has_catalog) { invalidateCatalog(); return; }
    const auto latest = vibe_pairing::snapshot();
    if (!canOperate() || !vibe_pairing::authorized_for_foreground() ||
        origin.url != latest.url || origin.token != latest.token) return;

    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto authenticated = vibe_pairing::snapshot();
        if (epoch != catalog_epoch_.load() || !canOperate() ||
            !vibe_pairing::authorized_for_foreground() || authenticated.url != origin.url ||
            authenticated.token != origin.token) return;
        if (!received_locale.empty()) (void)vibe_i18n::apply_bridge_locale(received_locale.c_str());
        const std::string identity = origin.url + "\n" + origin.token;
        if (identity != catalog_identity_) {
            providers_.clear();
            catalog_identity_ = identity;
            model_.sessions.clear();
            model_.tasks.clear();
            visible_task_id_.clear();
        }
        if (has_catalog) {
            const auto selected_id = selectedProviderLocked().id;
            const int previous_index = selected_provider_.load();
            const auto index = vibe_provider::replace(providers_, std::move(providers), selected_id);
            selected_provider_.store(static_cast<int>(index));
            // With no providers there is no session fetch to clear stale errors.
            if (providers_.empty()) model_.connection_error.clear();
            // Only an actual selection/index change invalidates BOOT gestures.
            if (previous_index != static_cast<int>(index) || selectedProviderLocked().id != selected_id)
                button_epoch_.fetch_add(1);
            if (selectedProviderLocked().id != selected_id) {
                model_.sessions.clear();
                model_.tasks.clear();
                model_.feedback.clear();
                model_.feedback_task_id.clear();
                model_.session_notice.clear();
                visible_task_id_.clear();
            }
        }
        voice_project_id_ = project;
        board_events_ = std::move(board_events);
        applyFirmwareOffer(firmware);
        if (can_voice) model_.voice_hint.clear();
        voice_available_.store(can_voice);
        // 桥接器重启会丢掉命令清单：config 里带 announce 标志时重新上报。
        if (announce_catalog) board_catalog_sent_ = false;
        config_loaded_.store(true);
        ++model_.revision;
    }
}

} // namespace esp_brookesia::apps
