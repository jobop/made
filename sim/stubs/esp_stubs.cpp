// sim/stubs/esp_stubs.cpp — ESP-IDF API 的 host 实现。
// 原则：能跑 UI 的最小行为。WiFi 永不连上、HTTP 永远失败、NVS 用内存。
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <SDL2/SDL.h>
#endif

namespace {
struct NvsKey { std::string ns; std::string key; char type; bool operator<(const NvsKey &o) const {
    return ns < o.ns || (ns == o.ns && (key < o.key || (key == o.key && type < o.type))); } };
static std::map<NvsKey, std::vector<uint8_t>> nvs_store;
static std::mutex nvs_mutex;

// 安卓上把 NVS 落到私有目录，重启不丢；PC sim 保持纯内存。
#ifdef __ANDROID__
static std::string nvs_file_path() {
    // 不能用 SDL_GetPrefPath：NVS 读写大多发生在 worker 线程（无 JNI env），
    // 它在安卓上会失败并静默回落 "."，写入 "/" 无权限 → 所有保存悄悄丢失。
    // SDL_AndroidGetInternalStoragePath 的值在主线程初始化后缓存，线程安全。
    const char *base = SDL_AndroidGetInternalStoragePath();
    const std::string dir = base ? base : "/data/local/tmp";
    if (base) mkdir(dir.c_str(), 0755);
    return dir + "/nvs.bin";
}
static void nvs_load_locked() {
    std::ifstream f(nvs_file_path(), std::ios::binary);
    if (!f) return;
    char magic[4] = {};
    f.read(magic, 4);
    if (memcmp(magic, "NVS1", 4) != 0) return;
    while (f) {
        uint32_t nl = 0, kl = 0, vl = 0; char type = 0;
        f.read((char *)&nl, 4); if (!f) break;
        std::string ns(nl, '\0'); f.read(ns.data(), nl);
        f.read((char *)&kl, 4);
        std::string key(kl, '\0'); f.read(key.data(), kl);
        f.read(&type, 1);
        f.read((char *)&vl, 4);
        std::vector<uint8_t> val(vl);
        if (vl) f.read((char *)val.data(), vl);
        nvs_store[{ns, key, type}] = std::move(val);
    }
}
static void nvs_save_locked() {
    const std::string save_path = nvs_file_path();
    std::ofstream f(save_path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write("NVS1", 4);
    for (const auto &[k, v] : nvs_store) {
        uint32_t nl = k.ns.size(), kl = k.key.size(), vl = v.size();
        f.write((char *)&nl, 4); f.write(k.ns.data(), nl);
        f.write((char *)&kl, 4); f.write(k.key.data(), kl);
        f.write(&k.type, 1);
        f.write((char *)&vl, 4); f.write((char *)v.data(), vl);
    }
}
#else
static void nvs_load_locked() {}
static void nvs_save_locked() {}
#endif
}

template <typename T>
static esp_err_t nvs_get_scalar_impl(nvs_handle_t h, const char *key, T *out)
{
    (void)h;
    std::lock_guard<std::mutex> lock(nvs_mutex);
    auto it = nvs_store.find({"", key, 'n'});
    if (it == nvs_store.end()) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, it->second.data(), sizeof(T));
    return ESP_OK;
}

template <typename T>
static esp_err_t nvs_set_scalar_impl(nvs_handle_t h, const char *key, T value)
{
    (void)h;
    std::lock_guard<std::mutex> lock(nvs_mutex);
    nvs_store[{"", key, 'n'}].assign((uint8_t *)&value, (uint8_t *)&value + sizeof(T));
    nvs_save_locked();
    return ESP_OK;
}

extern "C" {

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    default: return "ESP_ERR(unknown)";
    }
}

void esp_restart(void) { fprintf(stderr, "sim: esp_restart() called\n"); exit(0); }
uint32_t esp_get_free_heap_size(void) { return 200 * 1024; }
uint32_t esp_get_minimum_free_heap_size(void) { return 180 * 1024; }
uint32_t esp_random(void) { return (uint32_t)arc4random(); }

/* ---------------- esp_mac / esp_random / sntp（真机配对路径用） ---------------- */
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    // 固定本地管理位 MAC（02 = locally administered），"MADE" + 槽位号。
    // 同一台机器设备编号稳定，桥接器据此区分设备；不同 type 偏移避免串号。
    mac[0] = 0x02; mac[1] = 'M'; mac[2] = 'A'; mac[3] = 'D'; mac[4] = 'E';
    mac[5] = (uint8_t)(0x10 + (int)type);
    return ESP_OK;
}

void esp_fill_random(void *buf, size_t len)
{
    if (buf && len) arc4random_buf(buf, len);
}

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    // 宿主系统时钟本身可信（macOS/Android 维护），init 即视为同步完成。
    // 配置了 sync_cb 就同步回调一次，让 clock_synced 置位（HTTPS 证书时间校验放行）。
    if (config && config->sync_cb) {
        struct timeval now = {};
        gettimeofday(&now, nullptr);
        config->sync_cb(&now);
    }
    return ESP_OK;
}

/* ---------------- esp_timer ---------------- */
int64_t esp_timer_get_time(void)
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

struct esp_timer {
    esp_timer_cb_t callback = nullptr;
    void *arg = nullptr;
    std::thread worker;
    std::atomic<bool> running {false};
};

esp_err_t esp_timer_create(const esp_timer_args_t *args, esp_timer_handle_t *out)
{
    *out = new esp_timer {args->callback, args->arg, {}, {false}};
    return ESP_OK;
}

static void timer_loop(esp_timer *timer, uint64_t period_us, bool once)
{
    while (timer->running.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(once ? period_us : period_us));
        if (!timer->running.load()) break;
        timer->callback(timer->arg);
        if (once) break;
    }
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{
    timer->running = true;
    timer->worker = std::thread(timer_loop, timer, period_us, false);
    timer->worker.detach();
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay_us)
{
    timer->running = true;
    timer->worker = std::thread(timer_loop, timer, delay_us, true);
    timer->worker.detach();
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer) { timer->running = false; return ESP_OK; }
esp_err_t esp_timer_delete(esp_timer_handle_t timer) { timer->running = false; delete timer; return ESP_OK; }

/* ---------------- FreeRTOS ---------------- */
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                       void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)name; (void)stack_depth; (void)priority; (void)handle;
    std::thread(fn, arg).detach();
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                                   void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core)
{
    (void)core;
    return xTaskCreate(fn, name, stack_depth, arg, priority, handle);
}

void vTaskDelay(TickType_t ticks) { usleep((useconds_t)ticks * 1000); }
void vTaskDelete(TaskHandle_t handle) { (void)handle; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(esp_timer_get_time() / 1000LL); }
const char *pcTaskGetName(TaskHandle_t handle) { (void)handle; return "sim"; }

/* 任务通知：sim 单槽实现（句柄不区分，只承担任务间唤醒）。
 * 典型用户是提示音任务：xTaskNotifyWait(portMAX_DELAY) 阻塞等位。 */
static std::mutex g_notify_mu;
static std::condition_variable g_notify_cv;
static uint32_t g_notify_pending = 0;

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action)
{
    (void)task;
    if (action == eSetBits || action == eNoAction) {
        std::lock_guard<std::mutex> lock(g_notify_mu);
        g_notify_pending |= value;
        g_notify_cv.notify_all();
    }
    return pdPASS;
}

BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t *value, TickType_t wait)
{
    (void)clear_on_entry;
    std::unique_lock<std::mutex> lock(g_notify_mu);
    if (wait == portMAX_DELAY) {
        g_notify_cv.wait(lock, [] { return g_notify_pending != 0; });
    } else {
        g_notify_cv.wait_for(lock, std::chrono::milliseconds(wait),
                             [] { return g_notify_pending != 0; });
    }
    *value = g_notify_pending;
    g_notify_pending &= ~clear_on_exit;
    return *value != 0 ? pdTRUE : pdFALSE;
}

/* ---------------- NVS（内存键值） ---------------- */
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    static std::atomic<uint32_t> seq {1};
    *out = seq.fetch_add(1);
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    std::lock_guard<std::mutex> lock(nvs_mutex);
    bool erased = false;
    for (auto it = nvs_store.begin(); it != nvs_store.end();) {
        if (it->first.key == key) { it = nvs_store.erase(it); erased = true; } else ++it;
    }
    if (erased) nvs_save_locked();
    return erased ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) { (void)handle; std::lock_guard<std::mutex> lock(nvs_mutex); nvs_store.clear(); nvs_save_locked(); return ESP_OK; }

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *length)
{
    (void)h;
    std::lock_guard<std::mutex> lock(nvs_mutex);
    auto it = nvs_store.find({"", key, 's'});
    if (it == nvs_store.end()) return ESP_ERR_NVS_NOT_FOUND;
    const size_t need = it->second.size(); // 含结尾 0
    if (!out) { *length = need; return ESP_OK; }
    if (*length < need) { *length = need; return ESP_ERR_NO_MEM; }
    memcpy(out, it->second.data(), need);
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h;
    std::lock_guard<std::mutex> lock(nvs_mutex);
    const size_t len = strlen(value) + 1;
    nvs_store[{"", key, 's'}] = std::vector<uint8_t>((const uint8_t *)value, (const uint8_t *)value + len);
    nvs_save_locked();
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *length)
{
    (void)h;
    std::lock_guard<std::mutex> lock(nvs_mutex);
    auto it = nvs_store.find({"", key, 'b'});
    if (it == nvs_store.end()) return ESP_ERR_NVS_NOT_FOUND;
    const size_t need = it->second.size();
    if (!out) { *length = need; return ESP_OK; }
    if (*length < need) { *length = need; return ESP_ERR_NO_MEM; }
    memcpy(out, it->second.data(), need);
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t length)
{
    (void)h;
    std::lock_guard<std::mutex> lock(nvs_mutex);
    const uint8_t *p = (const uint8_t *)value;
    nvs_store[{"", key, 'b'}] = std::vector<uint8_t>(p, p + length);
    nvs_save_locked();
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *o) { return nvs_get_scalar_impl(h, k, o); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { return nvs_set_scalar_impl(h, k, v); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *o) { return nvs_get_scalar_impl(h, k, o); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { return nvs_set_scalar_impl(h, k, v); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *o) { return nvs_get_scalar_impl(h, k, o); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return nvs_set_scalar_impl(h, k, v); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *o) { return nvs_get_scalar_impl(h, k, o); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { return nvs_set_scalar_impl(h, k, v); }

esp_err_t nvs_flash_init(void) {
    std::lock_guard<std::mutex> lock(nvs_mutex);
    nvs_load_locked();
    return ESP_OK;
}
esp_err_t nvs_flash_erase(void) {
    std::lock_guard<std::mutex> lock(nvs_mutex);
    nvs_store.clear();
    nvs_save_locked();
    return ESP_OK;
}

/* ---------------- WiFi（永不连上） ---------------- */
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_stop(void) { return ESP_OK; }
esp_err_t esp_wifi_connect(void) { return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_get_mode(wifi_mode_t *mode) { *mode = WIFI_MODE_STA; return ESP_OK; }
esp_err_t esp_wifi_scan_start(void *config, bool block) { (void)config; (void)block; return ESP_ERR_INVALID_STATE; }
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number) { *number = 0; return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info)
{
    // MADE_SIM_WIFI=1 时报告已连上（否则网络轮询线程永远走「先连接 Wi-Fi」）。
    if (info) memset(info, 0, sizeof(*info));
    if (!getenv("MADE_SIM_WIFI")) return ESP_ERR_INVALID_STATE;
    if (info) snprintf(reinterpret_cast<char *>(info->ssid), sizeof(info->ssid), "SimWiFi");
    return ESP_OK;
}

/* ---------------- HTTP（真实 socket 实现：让 sim 连本机桩桥接器） ----------------
 * 只支持 http://（桩桥接器固定 http://127.0.0.1:8788）。连接失败返回
 * ESP_FAIL，行为与旧「永远失败」桩一致；连上则完整走请求/响应，
 * 并向 config.event_handler 派发 HTTP_EVENT_ON_DATA（VibeCoding::request
 * 靠它收集响应体）。fetch 主题用的 open/read/fetch_headers 一并实现。 */
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct esp_http_client {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::vector<std::string> headers;
    const esp_http_client_config_t *conf = nullptr;
    int status = 0;
    // open/read 状态（vibe_theme fetchPath 路径）
    std::string resp_body;
    size_t read_pos = 0;
    long long content_length = -1;
    bool opened = false;
};

namespace {
// 解析 http://host[:port]/path；失败返回 false。
bool sim_parse_url(const std::string &url, std::string &host, int &port, std::string &path)
{
    const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0) return false;
    const std::string rest = url.substr(scheme.size());
    const size_t slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        port = atoi(authority.c_str() + colon + 1);
    } else {
        host = authority;
        port = 80;
    }
    return !host.empty() && port > 0;
}
} // namespace

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    if (!config || !config->url) return nullptr;
    // config.url 指向调用方临时 string，这里拷贝一份。
    // method 必须读 config.method（真机 IDF 行为）：vibe_voice 上传只设
    // config.method=HTTP_METHOD_POST，从不调 set_method，桩若硬编码 GET
    // 桥接器会回 404 未找到接口。
    static const char *kMethodNames[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "PATCH"};
    const auto m = static_cast<esp_http_client_method_t>(
        config->method < HTTP_METHOD_PATCH ? config->method : HTTP_METHOD_GET);
    return new esp_http_client{/*.url=*/std::string(config->url), /*.method=*/kMethodNames[m],
                               /*.body=*/{}, /*.headers=*/{}, /*.conf=*/config,
                               /*.status=*/0, /*.resp_body=*/{}, /*.read_pos=*/0,
                               /*.content_length=*/-1, /*.opened=*/false};
}

static void sim_fire_data(esp_http_client_handle_t client, const char *data, int len)
{
    if (!client->conf || !client->conf->event_handler || len <= 0) return;
    esp_http_client_event_t ev {};
    ev.event_id = HTTP_EVENT_ON_DATA;
    ev.client = client;
    ev.user_data = client->conf->user_data;
    ev.data = const_cast<char *>(data);
    ev.data_len = len;
    client->conf->event_handler(&ev);
}

// 建连 + 发请求 + 收完整响应（头部解析出状态码与 Content-Length）。
static bool sim_http_exchange(esp_http_client_handle_t client)
{
    std::string host, path;
    int port = 0;
    if (!sim_parse_url(client->url, host, port, path)) return false;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) return false;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }
    int nosigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    std::string req = client->method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    for (const auto &h : client->headers) req += h + "\r\n";
    if (!client->body.empty())
        req += "Content-Length: " + std::to_string(client->body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    if (!client->body.empty()) req += client->body;
    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { close(fd); return false; }
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, static_cast<size_t>(n));
    close(fd);

    const size_t head_end = raw.find("\r\n\r\n");
    std::string lower_head = head_end == std::string::npos ? raw : raw.substr(0, head_end);
    for (auto &ch : lower_head) ch = (char)tolower((unsigned char)ch);
    const bool chunked = lower_head.find("transfer-encoding: chunked") != std::string::npos;
    const std::string head = head_end == std::string::npos ? raw : raw.substr(0, head_end);
    std::string body_raw = head_end == std::string::npos ? std::string() : raw.substr(head_end + 4);
    // 状态码
    if (head.rfind("HTTP/", 0) == 0) {
        const size_t sp = head.find(' ');
        client->status = sp == std::string::npos ? 0 : atoi(head.c_str() + sp + 1);
    }
    if (chunked) {
        // Node 侧常用 chunked；真机 esp_http_client 会自动解帧，这里必须同样
        // 解掉 hex 长度行，否则响应体带 "92\r\n" 前缀，cJSON 直接解析失败。
        std::string decoded;
        size_t pos = 0;
        while (pos < body_raw.size()) {
            const size_t line_end = body_raw.find("\r\n", pos);
            if (line_end == std::string::npos) break;
            const long chunk_size = strtol(body_raw.c_str() + pos, nullptr, 16);
            if (chunk_size <= 0) break;
            const size_t data_start = line_end + 2;
            if (data_start + (size_t)chunk_size > body_raw.size()) {
                decoded.append(body_raw, data_start, std::string::npos);
                break;
            }
            decoded.append(body_raw, data_start, (size_t)chunk_size);
            pos = data_start + (size_t)chunk_size + 2; // 跳过数据后的 CRLF
        }
        client->resp_body = std::move(decoded);
        client->content_length = -1;
    } else {
        client->resp_body = std::move(body_raw);
        const size_t cl = head.find("Content-Length:");
        if (cl != std::string::npos) client->content_length = atoll(head.c_str() + cl + 15);
    }
    return client->status > 0;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    if (!client) return ESP_FAIL;
    if (!sim_http_exchange(client)) return ESP_FAIL;
    sim_fire_data(client, client->resp_body.data(), static_cast<int>(client->resp_body.size()));
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    delete client;
    return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v)
{ if (c && k && v) c->headers.push_back(std::string(k) + ": " + v); return ESP_OK; }
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *k)
{ (void)c; (void)k; return ESP_OK; }
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m)
{
    if (!c) return ESP_FAIL;
    static const char *names[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "PATCH"};
    c->method = names[m < HTTP_METHOD_PATCH ? m : HTTP_METHOD_GET];
    return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *d, int len)
{ if (c) c->body = (d && len > 0) ? std::string(d, static_cast<size_t>(len)) : std::string(); return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t client) { return client ? client->status : 0; }
int esp_http_client_get_content_length(esp_http_client_handle_t c) { return c ? static_cast<int>(c->content_length) : -1; }

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    (void)write_len;
    if (!c) return ESP_FAIL;
    c->read_pos = 0;
    c->content_length = -1;
    c->opened = sim_http_exchange(c);
    return c->opened ? ESP_OK : ESP_FAIL;
}
int esp_http_client_read(esp_http_client_handle_t c, char *buffer, int len)
{
    if (!c || !c->opened || !buffer || len <= 0 || c->read_pos >= c->resp_body.size()) return 0;
    const size_t remain = c->resp_body.size() - c->read_pos;
    const size_t take = remain < static_cast<size_t>(len) ? remain : static_cast<size_t>(len);
    memcpy(buffer, c->resp_body.data() + c->read_pos, take);
    c->read_pos += take;
    return static_cast<int>(take);
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { if (c) c->opened = false; return ESP_OK; }
const char *esp_http_client_get_url(esp_http_client_handle_t c) { return c ? c->url.c_str() : ""; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{ return c ? c->content_length : -1; }

esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }

/* ---------------- GPIO（BOOT 按钮永远松开） ---------------- */
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
int gpio_get_level(gpio_num_t gpio_num)
{
    // 宿主 BOOT 键：marker 文件存在 = 按下。桌面端 /tmp/vibe_boot，
    // Android 端 /data/local/tmp/vibe_boot（adb touch/rm 控制按住与松开）。
    if (gpio_num == GPIO_NUM_0) {
#ifdef __ANDROID__
        static const char *marker = "/data/local/tmp/vibe_boot";
#else
        static const char *marker = "/tmp/vibe_boot";
#endif
        struct stat st;
        return stat(marker, &st) == 0 ? 0 : 1;
    }
    return 1;
}
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level) { (void)gpio_num; (void)level; return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t gpio_num) { (void)gpio_num; return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t gpio_num, int mode) { (void)gpio_num; (void)mode; return ESP_OK; }

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *) { return nullptr; }
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *out)
{
    if (out) *out = 1;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t) { return ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t) { return ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t) { return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *) { return ESP_OK; }

static const mbedtls_md_info_t sim_md_info = {};
void mbedtls_md_init(mbedtls_md_context_t *ctx) { if (ctx) ctx->dummy = 0; }
void mbedtls_md_free(mbedtls_md_context_t *ctx) { if (ctx) ctx->dummy = 0; }
const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t) { return &sim_md_info; }
int mbedtls_md_setup(mbedtls_md_context_t *, const mbedtls_md_info_t *, int) { return 0; }
int mbedtls_md_starts(mbedtls_md_context_t *) { return 0; }
int mbedtls_md_update(mbedtls_md_context_t *, const unsigned char *, size_t) { return 0; }
int mbedtls_md_finish(mbedtls_md_context_t *, unsigned char *output)
{
    if (output) memset(output, 0, 32);
    return 0;
}

/* ---------------- SHA-256 / HMAC-SHA256（真实实现） ----------------
 * /pair/verify 的 HMAC 回签与配对应答签名都依赖 mbedtls_md_hmac，
 * 假桩会让握手永远失败，这里给出独立的 SHA-256 实现。 */
namespace {

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t buflen;
};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

inline uint32_t rotr32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

void sha256_init(Sha256Ctx &c)
{
    static const uint32_t init[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c.state, init, sizeof(init));
    c.bitlen = 0;
    c.buflen = 0;
}

void sha256_block(Sha256Ctx &c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i-15],7) ^ rotr32(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2],17) ^ rotr32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c.state[0],b=c.state[1],d=c.state[3],e=c.state[4];
    uint32_t cc=c.state[2],f=c.state[5],g=c.state[6],h=c.state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c.state[0]+=a; c.state[1]+=b; c.state[2]+=cc; c.state[3]+=d;
    c.state[4]+=e; c.state[5]+=f; c.state[6]+=g; c.state[7]+=h;
}

void sha256_update(Sha256Ctx &c, const uint8_t *data, size_t len)
{
    c.bitlen += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - c.buflen;
        if (take > len) take = len;
        memcpy(c.buffer + c.buflen, data, take);
        c.buflen += take; data += take; len -= take;
        if (c.buflen == 64) { sha256_block(c, c.buffer); c.buflen = 0; }
    }
}

void sha256_final(Sha256Ctx &c, uint8_t out[32])
{
    uint64_t bitlen = c.bitlen;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c.buflen != 56) sha256_update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; ++i) lenbuf[i] = (uint8_t)(bitlen >> (56 - i * 8));
    // bitlen 已在 update 里累加，改用局部状态写长度避免二次计数。
    memcpy(c.buffer + 56, lenbuf, 8);
    sha256_block(c, c.buffer);
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (uint8_t)(c.state[i] >> 24);
        out[i*4+1] = (uint8_t)(c.state[i] >> 16);
        out[i*4+2] = (uint8_t)(c.state[i] >> 8);
        out[i*4+3] = (uint8_t)(c.state[i]);
    }
}

void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    Sha256Ctx c;
    sha256_init(c);
    sha256_update(c, data, len);
    sha256_final(c, out);
}

} // namespace

int mbedtls_md_hmac(const mbedtls_md_info_t *, const unsigned char *key, size_t keylen,
                    const unsigned char *input, size_t ilen, unsigned char *output)
{
    if (!key || !input || !output) return MBEDTLS_ERR_MD_BAD_INPUT_DATA;
    uint8_t key_block[64] = {};
    if (keylen > 64) sha256(key, keylen, key_block);
    else memcpy(key_block, key, keylen);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5c;
    }
    uint8_t inner[32];
    Sha256Ctx c;
    sha256_init(c);
    sha256_update(c, ipad, 64);
    sha256_update(c, input, ilen);
    sha256_final(c, inner);
    sha256_init(c);
    sha256_update(c, opad, 64);
    sha256_update(c, inner, 32);
    sha256_final(c, output);
    return 0;
}

esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t) { return ESP_OK; }
esp_err_t esp_sleep_enable_ext0_wakeup(gpio_num_t, int) { return ESP_OK; }
void esp_deep_sleep_start(void) { printf("sim: deep sleep skipped\n"); }
esp_err_t rtc_gpio_init(gpio_num_t) { return ESP_OK; }
esp_err_t rtc_gpio_set_direction(gpio_num_t, rtc_gpio_mode_t) { return ESP_OK; }
esp_err_t rtc_gpio_pullup_en(gpio_num_t) { return ESP_OK; }
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t) { return ESP_OK; }

int bsp_display_backlight_on(void) { printf("sim: backlight on\n"); return 0; }
int bsp_display_backlight_off(void) { printf("sim: backlight off\n"); return 0; }

// 运行时屏幕尺寸（安卓 main() 启动时设置）
int g_made_screen_w = 0;
int g_made_screen_h = 0;

} // extern "C"
