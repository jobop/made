// SPDX-License-Identifier: Apache-2.0
#include "vibe_phone_setup.hpp"
#include "phone_setup_protocol.hpp"
#include "phone_setup_socket.hpp"
#include "vibe_wifi.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <unistd.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

namespace vibe_phone_setup {
namespace {
constexpr char kAddress[] = "192.168.8.1";
constexpr char kUrl[] = "http://192.168.8.1";
constexpr char kOrigin[] = "http://192.168.8.1";
std::mutex state_mutex;
Snapshot state;
InitialConfig initial_config;
Submission pending;
std::string nonce, page;
bool english = false, pending_available = false;
uint32_t generation = 0;
int64_t deadline = 0, submitted_ready_at = 0;
httpd_handle_t http_server = nullptr;
std::atomic<bool> dns_running{false};
int dns_socket = -1;
SemaphoreHandle_t dns_stopped = nullptr;

const char *localized(const char *zh, const char *en) { return english ? en : zh; }

void clear_secret(std::string &value) {
    if (!value.empty()) {
        volatile char *bytes = &value[0];
        for (size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
    }
    value.clear();
}

void clear_pending() {
    clear_secret(pending.wifi_password);
    clear_secret(pending.receiver_password);
    pending = {};
    pending_available = false;
    submitted_ready_at = 0;
}

bool active_locked(int64_t now) {
    return state.phase == Phase::Ready && deadline > now;
}

bool ap_socket(int socket) {
    sockaddr_storage local{};
    socklen_t length = sizeof(local);
    return getsockname(socket, reinterpret_cast<sockaddr *>(&local), &length) == 0 &&
        detail::is_setup_address(reinterpret_cast<const sockaddr *>(&local), length);
}

esp_err_t open_connection(httpd_handle_t, int socket) {
    // APSTA may keep the old station online. Never expose this portal on STA.
    return ap_socket(socket) ? ESP_OK : ESP_FAIL;
}

void response_headers(httpd_req_t *request, const char *type) {
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(request, "Pragma", "no-cache");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
}

std::string header(httpd_req_t *request, const char *name, size_t maximum = 100) {
    const size_t length = httpd_req_get_hdr_value_len(request, name);
    if (!length || length > maximum) return {};
    std::string value(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, name, &value[0], value.size()) != ESP_OK) return {};
    value.resize(length);
    return value;
}

bool canonical_host(httpd_req_t *request) {
    const auto host = header(request, "Host");
    return host == kAddress || host == "192.168.8.1:80";
}

esp_err_t json_response(httpd_req_t *request, const char *status, const char *message, bool ok) {
    response_headers(request, "application/json; charset=utf-8");
    httpd_resp_set_status(request, status);
    cJSON *body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(body, "ok", ok);
    if (message) cJSON_AddStringToObject(body, "message", message);
    char *serialized = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!serialized) return ESP_ERR_NO_MEM;
    const esp_err_t result = httpd_resp_send(request, serialized, HTTPD_RESP_USE_STRLEN);
    cJSON_free(serialized);
    return result;
}

esp_err_t reject(httpd_req_t *request, const char *status, const char *message) {
    (void)json_response(request, status, message, false);
    // Some rejections leave an unread body. Close instead of reusing its socket.
    return ESP_FAIL;
}

esp_err_t get_page(httpd_req_t *request) {
    if (!ap_socket(httpd_req_to_sockfd(request))) return ESP_FAIL;
    if (!canonical_host(request) || std::strcmp(request->uri, "/") != 0) {
        response_headers(request, "text/plain; charset=utf-8");
        httpd_resp_set_status(request, "302 Found");
        httpd_resp_set_hdr(request, "Location", "http://192.168.8.1/");
        return httpd_resp_send(request, "Open http://192.168.8.1/", HTTPD_RESP_USE_STRLEN);
    }
    std::string body;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (active_locked(esp_timer_get_time())) body = page;
    }
    if (body.empty()) return reject(request, "410 Gone", localized(
        "此配置入口已关闭，请在码得重新打开手机配置。", "Setup is closed. Reopen phone setup on made."));
    response_headers(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t configure(httpd_req_t *request) {
    if (!ap_socket(httpd_req_to_sockfd(request)) || !canonical_host(request)) return ESP_FAIL;
    const auto origin = header(request, "Origin");
    if (httpd_req_get_hdr_value_len(request, "Origin") && origin != kOrigin && origin != "http://192.168.8.1:80")
        return reject(request, "403 Forbidden", "Origin rejected");
    if (!detail::json_content_type(header(request, "Content-Type")))
        return reject(request, "415 Unsupported Media Type", "Use application/json");
    if (request->content_len <= 0 || request->content_len > detail::kMaxBody)
        return reject(request, "413 Content Too Large", "Request must be 1–2048 bytes");
    if (httpd_req_get_hdr_value_len(request, "Transfer-Encoding"))
        return reject(request, "400 Bad Request", "Chunked requests are not supported");
    std::string session_nonce;
    InitialConfig initial;
    uint32_t session_generation;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!active_locked(esp_timer_get_time())) return reject(request, "410 Gone", localized(
            "入口已关闭或已提交，请在码得重新打开。", "Setup is closed or already submitted. Reopen it on made."));
        session_generation = generation;
        session_nonce = nonce;
        initial = initial_config;
    }
    std::string body(request->content_len, '\0');
    size_t received = 0;
    const int64_t receive_until = esp_timer_get_time() + 5LL * 1000000;
    while (received < body.size()) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (session_generation != generation || !active_locked(esp_timer_get_time())) {
                clear_secret(body);
                return ESP_FAIL;
            }
        }
        if (esp_timer_get_time() >= receive_until) {
            clear_secret(body);
            return reject(request, "408 Request Timeout", localized("接收超时，请重试。", "Request timed out. Retry."));
        }
        const int count = httpd_req_recv(request, &body[received], body.size() - received);
        if (count <= 0) {
            clear_secret(body);
            return reject(request, "408 Request Timeout", localized("接收超时，请重试。", "Request timed out. Retry."));
        }
        if (esp_timer_get_time() >= receive_until) {
            clear_secret(body);
            return reject(request, "408 Request Timeout", localized("接收超时，请重试。", "Request timed out. Retry."));
        }
        received += count;
    }
    Submission result;
    const auto validation = detail::parse_submission(body, session_nonce, initial, result);
    clear_secret(body);
    clear_secret(session_nonce);
    if (validation != detail::ValidationError::None)
        return reject(request, validation == detail::ValidationError::InvalidNonce ? "403 Forbidden" : "400 Bad Request",
                      detail::error_message(validation, english));
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (generation != session_generation || !active_locked(esp_timer_get_time())) {
            clear_secret(result.wifi_password); clear_secret(result.receiver_password);
            return reject(request, "410 Gone", localized("入口已失效，请重新打开。", "Setup expired. Reopen it on made."));
        }
        // Reserve the one submission before sending. No Wi-Fi or pairing work here.
        pending = std::move(result);
        pending_available = true;
        submitted_ready_at = 0;
        state.phase = Phase::Submitted;
        state.message = localized("收到配置，正在应用…", "Configuration received. Applying…");
        ++state.revision;
    }
    const esp_err_t sent = json_response(request, "200 OK", nullptr, true);
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (session_generation == generation && state.phase == Phase::Submitted) {
            if (sent == ESP_OK) submitted_ready_at = esp_timer_get_time() + detail::kResponseGraceUs;
            else {
                clear_pending();
                state.phase = Phase::Ready;
                state.message = localized("手机未收到响应，请重试。", "Phone did not receive the response. Retry.");
                ++state.revision;
            }
        }
    }
    return sent;
}


void dns_task(void *argument) {
    const int socket = static_cast<int>(reinterpret_cast<intptr_t>(argument));
    uint8_t query[512], response[512];
    while (dns_running.load()) {
        sockaddr_in remote{};
        socklen_t remote_size = sizeof(remote);
        const int size = recvfrom(socket, query, sizeof(query), 0,
            reinterpret_cast<sockaddr *>(&remote), &remote_size);
        if (size <= 0 || !dns_running.load()) continue;
        // Responses stay on the temporary AP subnet; DNS itself is bound to AP.
        if ((ntohl(remote.sin_addr.s_addr) & 0xffffff00UL) != 0xc0a80800UL) continue;
        const size_t bytes = detail::dns_reply(query, static_cast<size_t>(size), response, sizeof(response));
        if (bytes) (void)sendto(socket, response, bytes, 0, reinterpret_cast<sockaddr *>(&remote), remote_size);
    }
    xSemaphoreGive(dns_stopped);
    vTaskDelete(nullptr);
}

esp_err_t start_dns() {
    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_socket < 0) return ESP_FAIL;
    sockaddr_in local{};
    local.sin_family = AF_INET; local.sin_port = htons(53); local.sin_addr.s_addr = htonl(0xc0a80801UL);
    timeval timeout{0, 200000};
    if (setsockopt(dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(dns_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(dns_socket, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        close(dns_socket); dns_socket = -1; return ESP_FAIL;
    }
    dns_stopped = xSemaphoreCreateBinary();
    if (!dns_stopped) { close(dns_socket); dns_socket = -1; return ESP_ERR_NO_MEM; }
    dns_running.store(true);
    if (xTaskCreate(dns_task, "made_setup_dns", 3072,
                    reinterpret_cast<void *>(static_cast<intptr_t>(dns_socket)), 3, nullptr) != pdPASS) {
        dns_running.store(false);
        vSemaphoreDelete(dns_stopped); dns_stopped = nullptr;
        close(dns_socket); dns_socket = -1; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void stop_services() {
    // Invalidate first so an in-flight receive cannot queue a stale submission.
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        ++generation;
        deadline = 0;
        clear_pending();
        clear_secret(nonce);
        page.clear();
    }
    if (http_server) { httpd_stop(http_server); http_server = nullptr; }
    if (dns_socket >= 0) {
        dns_running.store(false);
        // Receive/send timeouts bound the wait to at most a few hundred ms.
        shutdown(dns_socket, SHUT_RDWR);
        xSemaphoreTake(dns_stopped, portMAX_DELAY);
        close(dns_socket); dns_socket = -1;
        vSemaphoreDelete(dns_stopped); dns_stopped = nullptr;
    }
    (void)vibe_wifi::stop_setup_ap();
}

esp_err_t start_failed(esp_err_t error) {
    stop_services();
    std::lock_guard<std::mutex> lock(state_mutex);
    state.phase = Phase::Error;
    clear_secret(state.password); state.ssid.clear(); state.url.clear(); state.qr_payload.clear();
    state.message = localized("手机配置启动失败，请稍后重试。", "Could not start phone setup. Please retry.");
    ++state.revision;
    return error;
}
} // namespace

esp_err_t start(bool use_english, const InitialConfig &initial) {
    stop();
    english = use_english;
    vibe_wifi::cancel_receiver_scan();
    vibe_wifi::SetupAccessPoint ap;
    esp_err_t error = ESP_ERR_INVALID_STATE;
    for (unsigned attempt = 0; attempt < 31; ++attempt) {
        error = vibe_wifi::start_setup_ap(ap);
        if (error != ESP_ERR_INVALID_STATE || attempt == 30) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (error != ESP_OK) return start_failed(error);
    if (ap.address != kUrl && ap.address != std::string(kUrl) + "/") return start_failed(ESP_ERR_INVALID_STATE);
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    constexpr char digits[] = "0123456789abcdef";
    std::string session_nonce;
    session_nonce.reserve(32);
    for (const uint8_t value : random) { session_nonce += digits[value >> 4]; session_nonce += digits[value & 15]; }
    const std::string content = detail::render_page(use_english, initial, session_nonce);
    if (content.empty()) return start_failed(ESP_ERR_NO_MEM);
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        ++generation;
        initial_config = initial;
        nonce = std::move(session_nonce);
        page = content;
        deadline = esp_timer_get_time() + detail::kSessionUs;
        state.phase = Phase::Ready;
        state.ssid = std::move(ap.ssid); state.password = std::move(ap.password); state.url = kUrl;
        state.qr_payload = "WIFI:T:WPA;S:" + detail::qr_escape(state.ssid) + ";P:" + detail::qr_escape(state.password) + ";;";
        state.message = localized("手机扫码连接热点，再填写配置。", "Scan with your phone, join the hotspot, then enter the settings.");
        ++state.revision;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80; config.ctrl_port = 32769;
    config.stack_size = 10240; config.max_open_sockets = 3;
    config.max_uri_handlers = 2; config.lru_purge_enable = true;
    config.recv_wait_timeout = 3; config.send_wait_timeout = 3;
    config.open_fn = open_connection;
    config.uri_match_fn = httpd_uri_match_wildcard;
    error = httpd_start(&http_server, &config);
    if (error != ESP_OK) return start_failed(error);
    httpd_uri_t submit_uri{}; submit_uri.uri = "/api/config"; submit_uri.method = HTTP_POST; submit_uri.handler = configure;
    error = httpd_register_uri_handler(http_server, &submit_uri);
    if (error != ESP_OK) return start_failed(error);
    httpd_uri_t page_uri{}; page_uri.uri = "/*"; page_uri.method = HTTP_GET; page_uri.handler = get_page;
    error = httpd_register_uri_handler(http_server, &page_uri);
    if (error != ESP_OK) return start_failed(error);
    error = start_dns();
    if (error != ESP_OK) return start_failed(error);
    return ESP_OK;
}

void stop() {
    stop_services();
    std::lock_guard<std::mutex> lock(state_mutex);
    clear_secret(state.password);
    state.ssid.clear(); state.url.clear(); state.qr_payload.clear(); state.message.clear();
    state.phase = Phase::Idle;
    initial_config = {};
    ++state.revision;
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return state;
}

bool take_submission(Submission &out) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (state.phase != Phase::Submitted || !pending_available || submitted_ready_at == 0 ||
        esp_timer_get_time() < submitted_ready_at) return false;
    out = std::move(pending);
    clear_pending();
    return true;
}

void tick() {
    bool expired;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        expired = deadline > 0 && esp_timer_get_time() >= deadline &&
            (state.phase == Phase::Ready || state.phase == Phase::Submitted);
        // Allow a final response its one-second delivery grace, plus one worker
        // interval to consume it. Submitted sessions never remain open forever.
        if (state.phase == Phase::Submitted && submitted_ready_at > 0 &&
            esp_timer_get_time() < submitted_ready_at + detail::kResponseGraceUs) expired = false;
    }
    if (!expired) return;
    stop_services();
    std::lock_guard<std::mutex> lock(state_mutex);
    clear_secret(state.password); state.ssid.clear(); state.url.clear(); state.qr_payload.clear();
    state.phase = Phase::Expired;
    state.message = localized("手机配置已超时，请重新打开。", "Phone setup expired. Open it again.");
    ++state.revision;
}
} // namespace vibe_phone_setup
