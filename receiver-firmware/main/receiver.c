#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "bootloader_random.h"
#include "receiver_password.h"

#define HTTP_PORT 8788
#define MAX_REQUEST_BYTES 1100000
#define MAX_RESPONSE_BYTES (256 * 1024)
#define RAW_CHUNK_BYTES 1024
#define MAX_LINE_BYTES 2048
#define REQUEST_TIMEOUT_US (30LL * 1000 * 1000)
#define VOICE_TIMEOUT_US (150LL * 1000 * 1000)

static const char *TAG = "vibe_receiver";
static const char *NVS_NAMESPACE = "vibe_receiver";
static char s_ssid[33];
static char s_password[64];
static uint32_t s_next_id = 1;
static SemaphoreHandle_t s_relay_lock;
static atomic_bool s_station_connected;

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

static bool serial_send_data(uint32_t id, const uint8_t *bytes, size_t length,
                             int64_t deadline)
{
    char encoded[((RAW_CHUNK_BYTES + 2) / 3) * 4 + 1];
    size_t encoded_length = 0;
    if (length > RAW_CHUNK_BYTES ||
        mbedtls_base64_encode((unsigned char *)encoded, sizeof(encoded),
                              &encoded_length, bytes, length) != 0) return false;
    encoded[encoded_length] = '\0';
    char line[MAX_LINE_BYTES];
    const int line_length = snprintf(line, sizeof(line),
        "{\"type\":\"data\",\"id\":%" PRIu32 ",\"chunk\":\"%s\"}", id, encoded);
    return line_length > 0 && line_length + 1 <= MAX_LINE_BYTES &&
           serial_write_line(line, (size_t)line_length, deadline);
}

static bool serial_send_end(uint32_t id, int64_t deadline)
{
    char line[64];
    const int length = snprintf(line, sizeof(line),
                                "{\"type\":\"end\",\"id\":%" PRIu32 "}", id);
    return length > 0 && length < sizeof(line) &&
           serial_write_line(line, (size_t)length, deadline);
}

static void serial_send_cancel(uint32_t id)
{
    char line[64];
    const int length = snprintf(line, sizeof(line),
                               "{\"type\":\"cancel\",\"id\":%" PRIu32 "}", id);
    if (length > 0 && length < sizeof(line)) {
        (void)serial_write_line(line, (size_t)length, deadline_after(1000000));
    }
}

static void serial_discard_pending(void)
{
    s_reader.used = 0;
    s_reader.offset = 0;
    uint8_t scratch[256];
    while (serial_read_bytes(scratch, sizeof(scratch), 0) > 0) { }
}

static bool serial_read_line(char *line, size_t capacity, int64_t deadline)
{
    size_t length = 0;
    while (true) {
        if (esp_timer_get_time() >= deadline || !serial_is_connected() ||
            !atomic_load(&s_station_connected)) return false;
        if (s_reader.offset == s_reader.used) {
            const int got = serial_read_bytes(s_reader.bytes,
                sizeof(s_reader.bytes), wait_ticks(deadline));
            if (got <= 0) continue;
            s_reader.offset = 0;
            s_reader.used = (size_t)got;
        }
        const char c = (char)s_reader.bytes[s_reader.offset++];
        if (c == '\n') {
            if (length > 0 && line[length - 1] == '\r') length--;
            line[length] = '\0';
            return length > 0;
        }
        if (length + 1 >= capacity) return false;
        line[length++] = c;
    }
}

static cJSON *serial_read_frame(int64_t deadline)
{
    char line[MAX_LINE_BYTES];
    if (!serial_read_line(line, sizeof(line), deadline)) return NULL;
    return cJSON_ParseWithLength(line, strlen(line));
}

static bool frame_has_id(const cJSON *frame, uint32_t id)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(frame, "id");
    if (!cJSON_IsNumber(value)) return false;
    const double number = cJSON_GetNumberValue(value);
    return number >= 0 && number <= UINT32_MAX && (uint32_t)number == id &&
           number == (double)(uint32_t)number;
}

static cJSON *serial_read_frame_for_id(uint32_t id, int64_t deadline)
{
    while (esp_timer_get_time() < deadline) {
        cJSON *frame = serial_read_frame(deadline);
        if (frame == NULL) return NULL;
        if (frame_has_id(frame, id)) return frame;
        // A timed-out earlier request may still have bytes in the CDC RX
        // queue. Its response cannot belong to the current HTTP request.
        cJSON_Delete(frame);
    }
    return NULL;
}

static const char *frame_type(const cJSON *frame)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(frame, "type");
    return cJSON_IsString(value) ? cJSON_GetStringValue(value) : NULL;
}

static bool serial_wait_ready(int64_t deadline)
{
    while (esp_timer_get_time() < deadline) {
        cJSON *frame = serial_read_frame(deadline);
        if (frame == NULL) return false;
        const char *type = frame_type(frame);
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(frame, "version");
        const bool ready = type != NULL && strcmp(type, "ready") == 0 &&
            cJSON_IsNumber(version) && version->valuedouble == 1;
        cJSON_Delete(frame);
        if (ready) return true;
    }
    return false;
}

static bool valid_content_type(const char *type)
{
    if (type == NULL || strlen(type) >= 128) return false;
    for (const char *p = type; *p; ++p) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) return false;
    }
    return true;
}

static const char *status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Response";
    }
}

static esp_err_t send_error(httpd_req_t *req, int status, const char *message)
{
    char status_line[64];
    snprintf(status_line, sizeof(status_line), "%d %s", status, status_reason(status));
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) return ESP_FAIL;
    cJSON_AddStringToObject(body, "error", message);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json == NULL) return ESP_FAIL;
    const esp_err_t result = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return result;
}

static esp_err_t relay_response(httpd_req_t *req, uint32_t id, int64_t deadline)
{
    cJSON *frame = serial_read_frame_for_id(id, deadline);
    if (frame == NULL) {
        if (!atomic_load(&s_station_connected)) return ESP_FAIL;
        return send_error(req, 504, "电脑接收端响应超时");
    }
    const cJSON *status_value = cJSON_GetObjectItemCaseSensitive(frame, "status");
    const cJSON *length_value = cJSON_GetObjectItemCaseSensitive(frame, "length");
    const cJSON *type_value = cJSON_GetObjectItemCaseSensitive(frame, "contentType");
    const char *type = cJSON_IsString(type_value) ? cJSON_GetStringValue(type_value) : NULL;
    const double declared = cJSON_IsNumber(length_value) ?
        cJSON_GetNumberValue(length_value) : -1;
    const int status = cJSON_IsNumber(status_value) ? status_value->valueint : -1;
    const bool valid = frame_has_id(frame, id) &&
        frame_type(frame) != NULL && strcmp(frame_type(frame), "response") == 0 &&
        status >= 100 && status <= 599 && declared >= 0 &&
        declared <= MAX_RESPONSE_BYTES && declared == (double)(size_t)declared &&
        valid_content_type(type);
    if (!valid) {
        cJSON_Delete(frame);
        return send_error(req, 502, "电脑返回无效响应帧");
    }
    const size_t expected = (size_t)declared;
    char content_type[128];
    memcpy(content_type, type, strlen(type) + 1);
    cJSON_Delete(frame);

    char status_line[64];
    snprintf(status_line, sizeof(status_line), "%d %s", status, status_reason(status));
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, content_type);
    size_t received = 0;
    while (true) {
        frame = serial_read_frame_for_id(id, deadline);
        if (frame == NULL) {
            cJSON_Delete(frame);
            return ESP_FAIL;
        }
        const char *kind = frame_type(frame);
        if (kind != NULL && strcmp(kind, "end") == 0) {
            cJSON_Delete(frame);
            if (received != expected) return ESP_FAIL;
            return httpd_resp_send_chunk(req, NULL, 0);
        }
        const cJSON *chunk_value = cJSON_GetObjectItemCaseSensitive(frame, "chunk");
        const char *chunk = cJSON_IsString(chunk_value) ? cJSON_GetStringValue(chunk_value) : NULL;
        if (kind == NULL || strcmp(kind, "data") != 0 || chunk == NULL ||
            strlen(chunk) > ((RAW_CHUNK_BYTES + 2) / 3) * 4) {
            cJSON_Delete(frame);
            return ESP_FAIL;
        }
        uint8_t decoded[RAW_CHUNK_BYTES];
        size_t decoded_length = 0;
        const int base64_result = mbedtls_base64_decode(decoded, sizeof(decoded),
            &decoded_length, (const unsigned char *)chunk, strlen(chunk));
        cJSON_Delete(frame);
        if (base64_result != 0 || decoded_length == 0 ||
            decoded_length > expected - received) return ESP_FAIL;
        if (httpd_resp_send_chunk(req, (const char *)decoded, decoded_length) != ESP_OK) return ESP_FAIL;
        received += decoded_length;
    }
}

static bool read_header(httpd_req_t *req, const char *name, char *value, size_t capacity)
{
    const size_t length = httpd_req_get_hdr_value_len(req, name);
    if (length >= capacity) return false;
    value[0] = '\0';
    return length == 0 || httpd_req_get_hdr_value_str(req, name, value, capacity) == ESP_OK;
}

static esp_err_t relay_request(httpd_req_t *req)
{
    const char *method = req->method == HTTP_GET ? "GET" :
                         req->method == HTTP_POST ? "POST" : NULL;
    if (method == NULL ||
        (strncmp(req->uri, "/pair/", 6) != 0 &&
         strncmp(req->uri, "/device/", 8) != 0)) {
        return send_error(req, 404, "接收端未提供此接口");
    }
    const size_t path_length = strnlen(req->uri, sizeof(req->uri));
    if (path_length == sizeof(req->uri)) {
        return send_error(req, 400, "请求路径或长度无效");
    }
    if (req->content_len > MAX_REQUEST_BYTES) {
        return send_error(req, 413, "请求体超过接收端限制");
    }
    char authorization[256];
    char content_type[128];
    if (!read_header(req, "Authorization", authorization, sizeof(authorization)) ||
        !read_header(req, "Content-Type", content_type, sizeof(content_type))) {
        return send_error(req, 431, "请求头过长");
    }
    if (xSemaphoreTake(s_relay_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        return send_error(req, 503, "接收端正在处理另一请求");
    }

    esp_err_t result = ESP_FAIL;
    const bool voice = req->method == HTTP_POST &&
        strncmp(req->uri, "/device/voice", 13) == 0 &&
        (req->uri[13] == '\0' || req->uri[13] == '?');
    const int64_t deadline = deadline_after(voice ? VOICE_TIMEOUT_US : REQUEST_TIMEOUT_US);
    const uint32_t id = s_next_id++;
    serial_discard_pending();
    // Require a live desktop process before sending the request body. This
    // also re-establishes the protocol if the desktop restarts while AP stays up.
    const int64_t handshake_deadline = deadline_after(3000000);
    if (!serial_send_hello(handshake_deadline) || !serial_wait_ready(handshake_deadline)) goto done;
    cJSON *request = cJSON_CreateObject();
    if (request == NULL) goto done;
    cJSON_AddStringToObject(request, "type", "request");
    cJSON_AddNumberToObject(request, "id", id);
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddStringToObject(request, "path", req->uri);
    cJSON_AddStringToObject(request, "authorization", authorization);
    cJSON_AddStringToObject(request, "contentType", content_type);
    cJSON_AddNumberToObject(request, "length", req->content_len);
    if (!serial_write_json(request, deadline)) goto done;

    size_t remaining = (size_t)req->content_len;
    uint8_t chunk[RAW_CHUNK_BYTES];
    while (remaining > 0) {
        if (esp_timer_get_time() >= deadline || !atomic_load(&s_station_connected)) goto done;
        const size_t wanted = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        const int got = httpd_req_recv(req, (char *)chunk, wanted);
        if (got <= 0 || !serial_send_data(id, chunk, (size_t)got, deadline)) goto done;
        remaining -= (size_t)got;
    }
    if (!serial_send_end(id, deadline)) goto done;
    result = relay_response(req, id, deadline);
    if (result != ESP_OK || !atomic_load(&s_station_connected)) serial_send_cancel(id);
    xSemaphoreGive(s_relay_lock);
    return result;

done:
    serial_send_cancel(id);
    xSemaphoreGive(s_relay_lock);
    if (!atomic_load(&s_station_connected)) return ESP_FAIL;
    return send_error(req, esp_timer_get_time() >= deadline ? 504 : 502,
                      "电脑未连接或 USB 转发失败");
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
            // Idle hello acknowledgements are not read by relay_request.
            // Drain the previous ready while holding the relay lock so an
            // idle receiver cannot fill USB RX or consume an active response.
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

static esp_err_t init_http(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.stack_size = 12288;
    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server start failed");
    const httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = relay_request};
    const httpd_uri_t post = {.uri = "/*", .method = HTTP_POST, .handler = relay_request};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get), TAG, "GET handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post), TAG, "POST handler failed");
    return ESP_OK;
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
    if (s_relay_lock == NULL) abort();
    ESP_ERROR_CHECK(init_hotspot());
    ESP_ERROR_CHECK(init_serial_transport());
    ESP_ERROR_CHECK(init_http());
    if (xTaskCreate(hello_task, "receiver_hello", 4096, NULL, 4, NULL) != pdPASS) abort();
}
