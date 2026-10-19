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
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
struct NvsKey { std::string ns; std::string key; char type; bool operator<(const NvsKey &o) const {
    return ns < o.ns || (ns == o.ns && (key < o.key || (key == o.key && type < o.type))); } };
static std::map<NvsKey, std::vector<uint8_t>> nvs_store;
static std::mutex nvs_mutex;
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
    return erased ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) { (void)handle; std::lock_guard<std::mutex> lock(nvs_mutex); nvs_store.clear(); return ESP_OK; }

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

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }

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
    return new esp_http_client{/*.url=*/std::string(config->url), /*.method=*/"GET",
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
    const std::string head = head_end == std::string::npos ? raw : raw.substr(0, head_end);
    client->resp_body = head_end == std::string::npos ? std::string() : raw.substr(head_end + 4);
    // 状态码
    if (head.rfind("HTTP/", 0) == 0) {
        const size_t sp = head.find(' ');
        client->status = sp == std::string::npos ? 0 : atoi(head.c_str() + sp + 1);
    }
    // Content-Length（分块编码就不裁了，桩服务器不会用）
    if (head.find("Transfer-Encoding: chunked") == std::string::npos) {
        const size_t cl = head.find("Content-Length:");
        if (cl != std::string::npos) client->content_length = atoll(head.c_str() + cl + 15);
    } else {
        client->content_length = -1;
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
int gpio_get_level(gpio_num_t gpio_num) { (void)gpio_num; return 1; }
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

esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t) { return ESP_OK; }
esp_err_t esp_sleep_enable_ext0_wakeup(gpio_num_t, int) { return ESP_OK; }
void esp_deep_sleep_start(void) { printf("sim: deep sleep skipped\n"); }
esp_err_t rtc_gpio_init(gpio_num_t) { return ESP_OK; }
esp_err_t rtc_gpio_set_direction(gpio_num_t, rtc_gpio_mode_t) { return ESP_OK; }
esp_err_t rtc_gpio_pullup_en(gpio_num_t) { return ESP_OK; }
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t) { return ESP_OK; }

int bsp_display_backlight_on(void) { printf("sim: backlight on\n"); return 0; }
int bsp_display_backlight_off(void) { printf("sim: backlight off\n"); return 0; }

} // extern "C"
