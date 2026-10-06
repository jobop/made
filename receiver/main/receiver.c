#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "bootloader_random.h"
#include "receiver_password.h"

#define RELAY_PORT 8788
#define SPLICE_BYTES 1024
#define MAX_LINE_BYTES 2048

static const char *TAG = "vibe_receiver";
static const char *NVS_NAMESPACE = "vibe_receiver";
static char s_ssid[33];
static char s_password[64];
static SemaphoreHandle_t s_relay_lock;
static SemaphoreHandle_t s_uplink_done;
static atomic_bool s_station_connected;
static atomic_bool s_splice_open;
static int s_client = -1;
static uint8_t s_down[SPLICE_BYTES];
static uint8_t s_up[SPLICE_BYTES];

typedef struct {
    uint8_t bytes[512];
    size_t used;
    size_t offset;
} serial_reader_t;

static serial_reader_t s_reader;

static bool serial_is_connected(void)
{
    // 双通道常开：UART 无主机在场信号，原生 USB 断开时写入会被跳过。
    // 真正的在场判定由 hello → ready 握手完成。
    return true;
}

static int serial_read_bytes(void *buffer, size_t size, TickType_t ticks)
{
    // 两个通道都可能连着电脑：先听原生 USB，再听 UART0，各自分一半超时。
    const TickType_t half = (ticks / 2) ? (ticks / 2) : 1;
    const int from_usb = usb_serial_jtag_read_bytes(buffer, size, half);
    if (from_usb > 0) return from_usb;
    return uart_read_bytes(UART_NUM_0, buffer, size, half);
}

static int serial_write_bytes(const void *buffer, size_t size, TickType_t ticks)
{
    // 广播到两个通道：UART 永远写（无主机在场信号，无人接收也无副作用），
    // 原生 USB 仅在主机已连接时写，避免无人读取时阻塞。
    const TickType_t half = (ticks / 2) ? (ticks / 2) : 1;
    uart_write_bytes(UART_NUM_0, buffer, size);
    if (usb_serial_jtag_is_connected()) {
        return usb_serial_jtag_write_bytes(buffer, size, half);
    }
    return (int)size;
}

static int64_t deadline_after(int64_t duration_us)
{
    return esp_timer_get_time() + duration_us;
}

static TickType_t wait_ticks(int64_t deadline)
{
    const int64_t remaining_ms = (deadline - esp_timer_get_time() + 999) / 1000;
    if (remaining_ms <= 0) return 0;
    const uint32_t slice_ms = remaining_ms > 100 ? 100 : (uint32_t)remaining_ms;
    TickType_t ticks = pdMS_TO_TICKS(slice_ms);
    return ticks == 0 ? 1 : ticks;
}

static bool serial_write_all(const char *data, size_t length, int64_t deadline)
{
    size_t sent = 0;
    while (sent < length) {
        if (!serial_is_connected()) return false;
        const TickType_t ticks = wait_ticks(deadline);
        if (ticks == 0) return false;
        const int wrote = serial_write_bytes(data + sent, length - sent, ticks);
        if (wrote > 0) sent += (size_t)wrote;
    }
    return true;
}

static bool serial_write_line(const char *json, size_t length, int64_t deadline)
{
    if (length + 1 > MAX_LINE_BYTES) return false;
    char line[MAX_LINE_BYTES];
    memcpy(line, json, length);
    line[length] = '\n';
    return serial_write_all(line, length + 1, deadline);
}

static bool serial_write_json(cJSON *object, int64_t deadline)
{
    if (object == NULL) return false;
    char *json = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    if (json == NULL) return false;
    const bool okay = serial_write_line(json, strlen(json), deadline);
    cJSON_free(json);
    return okay;
}

static bool serial_send_hello(int64_t deadline)
{
    cJSON *hello = cJSON_CreateObject();
    if (hello == NULL) return false;
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddNumberToObject(hello, "version", 1);
    cJSON_AddStringToObject(hello, "ssid", s_ssid);
    cJSON_AddStringToObject(hello, "password", s_password);
    return serial_write_json(hello, deadline);
}

static void serial_discard_pending(void)
{
    s_reader.used = 0;
    s_reader.offset = 0;
    uint8_t scratch[256];
    while (serial_read_bytes(scratch, sizeof(scratch), 0) > 0) { }
}

static bool s_splice_usb;

static int splice_write(const uint8_t *data, size_t size)
{
    // 拼接开始时选定一路。电脑关掉串口后不能改走 UART：
    // uart_write_bytes 会一直堵到发送缓冲消化完，hello 也就永远发不出去。
    if (s_splice_usb) {
        if (!usb_serial_jtag_is_connected()) return -1;
        return usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(200));
    }
    return uart_tx_chars(UART_NUM_0, (const char *)data, size);
}

static int splice_read(uint8_t *data, size_t size)
{
    if (usb_serial_jtag_is_connected()) {
        return usb_serial_jtag_read_bytes(data, size, pdMS_TO_TICKS(100));
    }
    return uart_read_bytes(UART_NUM_0, data, size, pdMS_TO_TICKS(100));
}

static void tcp_to_usb_task(void *unused)
{
    (void)unused;
    while (atomic_load(&s_splice_open) && atomic_load(&s_station_connected)) {
        const int got = recv(s_client, s_down, sizeof(s_down), 0);
        if (got == 0) break;
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        size_t sent = 0;
        while (sent < (size_t)got && atomic_load(&s_splice_open)) {
            const int wrote = splice_write(s_down + sent, (size_t)got - sent);
            if (wrote > 0) {
                sent += (size_t)wrote;
                continue;
            }
            if (wrote < 0 || !atomic_load(&s_splice_open)) break;
            vTaskDelay(1);
        }
    }
    atomic_store(&s_splice_open, false);
    if (s_client >= 0) shutdown(s_client, SHUT_RDWR);
    xSemaphoreGive(s_uplink_done);
    vTaskDelete(NULL);
}

static void splice_usb_to_tcp(void)
{
    while (atomic_load(&s_splice_open) && atomic_load(&s_station_connected)) {
        const int got = splice_read(s_up, sizeof(s_up));
        if (got < 0) break;
        if (got == 0) continue;
        size_t sent = 0;
        while (sent < (size_t)got && atomic_load(&s_splice_open) &&
               atomic_load(&s_station_connected)) {
            // 发送超时后重试同一段，码得读得慢时仍然形成背压，但不会永久占住转发锁。
            const int wrote = send(s_client, s_up + sent, (size_t)got - sent, 0);
            if (wrote > 0) {
                sent += (size_t)wrote;
                continue;
            }
            if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            break;
        }
    }
    atomic_store(&s_splice_open, false);
    if (s_client >= 0) shutdown(s_client, SHUT_RDWR);
}

static void relay_task(void *unused)
{
    (void)unused;
    const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) abort();
    const int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(RELAY_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "TCP listen on %d failed", RELAY_PORT);
        abort();
    }
    ESP_LOGI(TAG, "Byte pipe listening on 192.168.4.1:%d", RELAY_PORT);
    while (true) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listen_fd, &readable);
        struct timeval wait = {.tv_sec = 1, .tv_usec = 0};
        if (select(listen_fd + 1, &readable, NULL, NULL, &wait) <= 0) continue;
        if (xSemaphoreTake(s_relay_lock, 0) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        struct sockaddr_in peer = {0};
        socklen_t peer_length = sizeof(peer);
        const int client = accept(listen_fd, (struct sockaddr *)&peer, &peer_length);
        if (client < 0) {
            xSemaphoreGive(s_relay_lock);
            continue;
        }
        const int nodelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        const struct timeval slice = {.tv_sec = 0, .tv_usec = 200000};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &slice, sizeof(slice));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &slice, sizeof(slice));
        serial_discard_pending();
        s_splice_usb = usb_serial_jtag_is_connected();
        atomic_store(&s_station_connected, true);
        while (xSemaphoreTake(s_uplink_done, 0) == pdTRUE) { }
        s_client = client;
        atomic_store(&s_splice_open, true);
        if (xTaskCreate(tcp_to_usb_task, "rx_tcp_usb", 3072, NULL, 5, NULL) != pdPASS) {
            atomic_store(&s_splice_open, false);
            s_client = -1;
            close(client);
            xSemaphoreGive(s_relay_lock);
            continue;
        }
        splice_usb_to_tcp();
        if (xSemaphoreTake(s_uplink_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "TCP-to-USB task did not stop");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        s_client = -1;
        close(client);
        xSemaphoreGive(s_relay_lock);
    }
}


static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) atomic_store(&s_station_connected, true);
    if (id == WIFI_EVENT_AP_STADISCONNECTED) atomic_store(&s_station_connected, false);
}

static void hello_task(void *unused)
{
    (void)unused;
    while (true) {
        if (serial_is_connected() &&
            xSemaphoreTake(s_relay_lock, 0) == pdTRUE) {
            // 拼接进行时拿不到锁，因此不会把 hello 插进码得的帧流。
            // 清掉上一轮 ready，避免空闲回复堆满 USB 接收缓冲。
            serial_discard_pending();
            (void)serial_send_hello(deadline_after(1000000));
            xSemaphoreGive(s_relay_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static esp_err_t init_hotspot(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init failed");
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (ap == NULL) return ESP_ERR_NO_MEM;

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ip.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    esp_err_t dhcp_error = esp_netif_dhcps_stop(ap);
    if (dhcp_error != ESP_OK && dhcp_error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return dhcp_error;
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap, &ip), TAG, "set AP address failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap), TAG, "start DHCP server failed");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                       wifi_event_handler, NULL), TAG, "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "AP mode failed");

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, mac), TAG, "read AP MAC failed");
    char default_ssid[sizeof(s_ssid)];
    snprintf(default_ssid, sizeof(default_ssid), "VibeReceiver-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open AP identity NVS failed");
    size_t ssid_length = sizeof(s_ssid);
    esp_err_t ssid_error = nvs_get_str(nvs, "ssid", s_ssid, &ssid_length);
    if (ssid_error == ESP_ERR_NVS_NOT_FOUND) {
        memcpy(s_ssid, default_ssid, strlen(default_ssid) + 1);
        ssid_error = nvs_set_str(nvs, "ssid", s_ssid);
        if (ssid_error == ESP_OK) ssid_error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ssid_error != ESP_OK || strncmp(s_ssid, "VibeReceiver-", 13) != 0 ||
        strlen(s_ssid) > 32) return ESP_ERR_INVALID_STATE;

    wifi_config_t config = {0};
    memcpy(config.ap.ssid, s_ssid, strlen(s_ssid));
    config.ap.ssid_len = strlen(s_ssid);
    memcpy(config.ap.password, s_password, strlen(s_password));
    config.ap.channel = 6;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 1;
    config.ap.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "AP config failed");
    memset(&config, 0, sizeof(config));
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start failed");
    ESP_LOGI(TAG, "Receiver AP %s at 192.168.4.1:8788", s_ssid);
    return ESP_OK;
}

static esp_err_t load_hotspot_password(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open NVS failed");
    size_t password_length = sizeof(s_password);
    esp_err_t err = nvs_get_str(nvs, "password", s_password, &password_length);
    const bool missing = err == ESP_ERR_NVS_NOT_FOUND;
    if (!missing && err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    if (!missing && (strlen(s_password) < 8 || strlen(s_password) > 63)) {
        nvs_close(nvs);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t password_version = 0;
    const esp_err_t version_error = nvs_get_u8(nvs, "password_ver", &password_version);
    const bool version_present = version_error == ESP_OK;
    if (!version_present && version_error != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return version_error;
    }
    const bool migrate = !missing &&
        receiver_password_needs_migration(s_password, version_present);
    err = ESP_OK;
    if (missing || migrate) {
        // The Wi-Fi radio is not yet initialized; temporarily enable the
        // hardware entropy source for the initial or once-migrated secret.
        bootloader_random_enable();
        while (!receiver_password_from_random(esp_random(), s_password)) { }
        bootloader_random_disable();
        err = nvs_set_str(nvs, "password", s_password);
    }
    // Write the password before its marker. If power is lost between writes,
    // an already-generated eight-digit value is preserved on the next boot.
    if (err == ESP_OK && (missing || !version_present)) {
        err = nvs_set_u8(nvs, "password_ver", RECEIVER_PASSWORD_VERSION);
    }
    if (err == ESP_OK && (missing || migrate || !version_present)) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    if (strlen(s_password) < 8 || strlen(s_password) > 63) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t init_serial_transport(void)
{
    // 双通道常开：UART0（GPIO21/20，可接 USB 转串口）与原生 USB-Serial/JTAG
    // 同时初始化。电脑用直连或转接头都能工作，无需重新烧录。
    const uart_config_t uart_config = {
        .baud_rate = 921600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_0, &uart_config), TAG, "UART config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM_0, 21, 20, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "UART pins failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_0, 16 * 1024, 16 * 1024, 0, NULL, 0),
                        TAG, "UART driver install failed");
    usb_serial_jtag_driver_config_t usb_config = {
        .rx_buffer_size = 16 * 1024,
        .tx_buffer_size = 16 * 1024,
    };
    return usb_serial_jtag_driver_install(&usb_config);
}



void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(load_hotspot_password());
    s_relay_lock = xSemaphoreCreateMutex();
    s_uplink_done = xSemaphoreCreateBinary();
    if (s_relay_lock == NULL || s_uplink_done == NULL) abort();
    ESP_ERROR_CHECK(init_hotspot());
    ESP_ERROR_CHECK(init_serial_transport());
    if (xTaskCreate(relay_task, "receiver_relay", 4096, NULL, 5, NULL) != pdPASS) abort();
    if (xTaskCreate(hello_task, "receiver_hello", 4096, NULL, 4, NULL) != pdPASS) abort();
}
