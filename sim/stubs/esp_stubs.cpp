// sim/stubs/esp_stubs.cpp — ESP-IDF API 的 host 实现。
// 原则：能跑 UI 的最小行为。WiFi 永不连上、HTTP 永远失败、NVS 用内存。
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"

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
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info) { memset(info, 0, sizeof(*info)); return ESP_ERR_INVALID_STATE; }

/* ---------------- HTTP（永远失败） ---------------- */
struct esp_http_client { int status; };

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    (void)config;
    return new esp_http_client {0};
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client) { (void)client; return ESP_FAIL; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { delete client; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) { (void)c; (void)k; (void)v; return ESP_OK; }
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *k) { (void)c; (void)k; return ESP_OK; }
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m) { (void)c; (void)m; return ESP_OK; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *d, int len) { (void)c; (void)d; (void)len; return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t client) { return client->status; }
int esp_http_client_get_content_length(esp_http_client_handle_t c) { (void)c; return -1; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int n) { (void)c; (void)n; return ESP_FAIL; }
int esp_http_client_read(esp_http_client_handle_t c, char *b, int l) { (void)c; (void)b; (void)l; return 0; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
const char *esp_http_client_get_url(esp_http_client_handle_t c) { (void)c; return ""; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return 0; }

esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }

/* ---------------- GPIO（BOOT 按钮永远松开） ---------------- */
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
int gpio_get_level(gpio_num_t gpio_num) { (void)gpio_num; return 1; }
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level) { (void)gpio_num; (void)level; return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t gpio_num) { (void)gpio_num; return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t gpio_num, int mode) { (void)gpio_num; (void)mode; return ESP_OK; }

int bsp_display_backlight_on(void) { printf("sim: backlight on\n"); return 0; }
int bsp_display_backlight_off(void) { printf("sim: backlight off\n"); return 0; }

} // extern "C"
