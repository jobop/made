#include "vibe_usb.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace vibe_usb {
namespace {

constexpr size_t kChunkBytes = 1024;
constexpr size_t kLineBytes = 2048;
constexpr size_t kMaxRequest = 1100000;
constexpr size_t kMaxResponse = 256 * 1024;
constexpr int64_t kHandshakeUs = 1500000;
constexpr int64_t kHelloIntervalUs = 3000000;
constexpr int kIoSliceMs = 20;

std::atomic<bool> initialized{false};
std::atomic<bool> initializing{false};
std::atomic<bool> enabled{false};
std::atomic<bool> ready{false};
std::atomic<bool> pending_bye{false};
std::atomic<uint32_t> epoch{0};
std::atomic<uint32_t> cancellation{0};
std::atomic<uint32_t> authorized_epoch{UINT32_MAX};
portMUX_TYPE authorization_lock = portMUX_INITIALIZER_UNLOCKED;
char authorized_bearer[129] = {};
SemaphoreHandle_t request_lock = nullptr;
TaskHandle_t hello_task_handle = nullptr;
// The serial reader, connection ID and sequence are protected by request_lock.
std::string connection_id;
char device_id[13] = {};
uint32_t next_id = 1;
uint8_t read_buffer[512];
size_t read_used = 0;
size_t read_offset = 0;

struct Operation {
    int64_t deadline;
    uint32_t cancellation_generation;
    const std::atomic<bool>* cancellation_flag = nullptr;
};

void invalidate_connection() {
    if (ready.exchange(false)) {
        revoke_authorization();
        epoch.fetch_add(1);
    }
}

bool cable_connected() {
    const bool present = usb_serial_jtag_is_connected();
    if (!present) invalidate_connection();
    return present;
}

esp_err_t operation_error(const Operation& operation) {
    if (!enabled.load() || cancellation.load() != operation.cancellation_generation ||
        (operation.cancellation_flag != nullptr && operation.cancellation_flag->load())) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cable_connected()) return ESP_ERR_INVALID_STATE;
    if (esp_timer_get_time() >= operation.deadline) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

TickType_t wait_ticks(int64_t deadline) {
    const int64_t remaining_ms = (deadline - esp_timer_get_time() + 999) / 1000;
    if (remaining_ms <= 0) return 0;
    return std::max<TickType_t>(1, pdMS_TO_TICKS(std::min<int64_t>(kIoSliceMs, remaining_ms)));
}

esp_err_t write_bytes(const char* bytes, size_t length, const Operation& operation,
                      bool control = false) {
    size_t sent = 0;
    while (sent < length) {
        const esp_err_t error = control ? (cable_connected() ? ESP_OK : ESP_ERR_INVALID_STATE)
                                        : operation_error(operation);
        if (error != ESP_OK) return error;
        const TickType_t ticks = wait_ticks(operation.deadline);
        if (ticks == 0) return ESP_ERR_TIMEOUT;
        const int wrote = usb_serial_jtag_write_bytes(bytes + sent, length - sent, ticks);
        if (wrote > 0) sent += static_cast<size_t>(wrote);
    }
    return ESP_OK;
}

esp_err_t write_line(const char* json, size_t length, const Operation& operation,
                     bool control = false) {
    if (length + 1 > kLineBytes) return ESP_ERR_INVALID_SIZE;
    const esp_err_t result = write_bytes(json, length, operation, control);
    return result == ESP_OK ? write_bytes("\n", 1, operation, control) : result;
}

esp_err_t write_json(cJSON* object, const Operation& operation) {
    if (object == nullptr) return ESP_ERR_NO_MEM;
    char* json = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    if (json == nullptr) return ESP_ERR_NO_MEM;
    const esp_err_t result = write_line(json, std::strlen(json), operation);
    cJSON_free(json);
    return result;
}

esp_err_t send_hello(const Operation& operation) {
    char line[192];
    const int length = std::snprintf(line, sizeof(line),
        "{\"type\":\"hello\",\"version\":1,\"mode\":\"direct\",\"deviceId\":\"%s\","
        "\"deviceName\":\"码得\"}", device_id);
    return write_line(line, static_cast<size_t>(length), operation);
}

void send_control(uint32_t id, bool bye) {
    // Terminate a partially transmitted line before cancel/bye. The receiver
    // rejects that incomplete frame and can parse the following control frame.
    char line[96];
    const int length = bye
        ? std::snprintf(line, sizeof(line), "\n{\"type\":\"bye\",\"mode\":\"direct\"}\n")
        : std::snprintf(line, sizeof(line), "\n{\"type\":\"cancel\",\"id\":%" PRIu32 "}\n", id);
    const Operation operation{esp_timer_get_time() + 100000, cancellation.load()};
    (void)write_bytes(line, static_cast<size_t>(length), operation, true);
}

void send_pending_bye() {
    if (pending_bye.exchange(false)) send_control(0, true);
}

void discard_pending() {
    read_used = 0;
    read_offset = 0;
    // Bounded even if the other endpoint continuously transmits junk.
    uint8_t scratch[512];
    for (size_t count = 0; count < 32; ++count) {
        if (usb_serial_jtag_read_bytes(scratch, sizeof(scratch), 0) <= 0) break;
    }
}

esp_err_t read_frame(cJSON*& frame, const Operation& operation) {
    frame = nullptr;
    char line[kLineBytes];
    size_t length = 0;
    for (;;) {
        const esp_err_t error = operation_error(operation);
        if (error != ESP_OK) return error;
        if (read_offset == read_used) {
            const int got = usb_serial_jtag_read_bytes(read_buffer, sizeof(read_buffer),
                                                       wait_ticks(operation.deadline));
            if (got <= 0) continue;
            read_offset = 0;
            read_used = static_cast<size_t>(got);
        }
        const char value = static_cast<char>(read_buffer[read_offset++]);
        if (value == '\n') {
            if (length > 0 && line[length - 1] == '\r') --length;
            if (length == 0) continue;
            line[length] = '\0';
            const char* parse_end = nullptr;
            frame = cJSON_ParseWithLengthOpts(line, length + 1, &parse_end, true);
            if (frame == nullptr || !cJSON_IsObject(frame)) {
                cJSON_Delete(frame);
                frame = nullptr;
                return ESP_ERR_INVALID_RESPONSE;
            }
            return ESP_OK;
        }
        // A JSON line plus its newline may not exceed 2048 bytes.
        if (value == '\0' || length + 1 >= kLineBytes) return ESP_ERR_INVALID_SIZE;
        line[length++] = value;
    }
}

const char* string_field(const cJSON* frame, const char* name) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(frame, name);
    return cJSON_IsString(value) ? cJSON_GetStringValue(value) : nullptr;
}

bool type_is(const cJSON* frame, const char* type) {
    const char* actual = string_field(frame, "type");
    return actual != nullptr && std::strcmp(actual, type) == 0;
}

bool number_field(const cJSON* frame, const char* name, double minimum,
                  double maximum, double& value) {
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(frame, name);
    if (!cJSON_IsNumber(field)) return false;
    value = cJSON_GetNumberValue(field);
    return std::isfinite(value) && value >= minimum && value <= maximum &&
           std::floor(value) == value;
}

bool valid_text(const char* value, size_t maximum) {
    if (value == nullptr || std::strlen(value) > maximum) return false;
    for (const char* c = value; *c; ++c) {
        if (static_cast<unsigned char>(*c) < 0x20 || static_cast<unsigned char>(*c) > 0x7e) return false;
    }
    return true;
}

bool accept_ready(const cJSON* frame) {
    double version;
    if (!type_is(frame, "ready") || !number_field(frame, "version", 1, 1, version)) return false;
    const char* id = string_field(frame, "connectionId");
    const size_t length = id == nullptr ? 0 : std::strlen(id);
    if (length != 32 && length != 36) return false;
    for (size_t index = 0; index < length; ++index) {
        if (length == 36 && (index == 8 || index == 13 || index == 18 || index == 23)) {
            if (id[index] != '-') return false;
        } else if (!((id[index] >= '0' && id[index] <= '9') ||
                     (id[index] >= 'a' && id[index] <= 'f') ||
                     (id[index] >= 'A' && id[index] <= 'F'))) return false;
    }
    if (connection_id != id) {
        revoke_authorization();
        connection_id = id;
        epoch.fetch_add(1);
    }
    ready.store(true);
    return true;
}

esp_err_t handshake(const Operation& operation) {
    discard_pending();
    esp_err_t result = send_hello(operation);
    while (result == ESP_OK) {
        cJSON* frame = nullptr;
        result = read_frame(frame, operation);
        if (result != ESP_OK) break;
        const bool accepted = accept_ready(frame);
        cJSON_Delete(frame);
        if (accepted) return ESP_OK;
        // Responses from an earlier canceled request are deliberately ignored.
    }
    invalidate_connection();
    return result;
}

esp_err_t read_request_frame(uint32_t id, uint32_t request_epoch, cJSON*& frame,
                             const Operation& operation) {
    for (;;) {
        esp_err_t result = read_frame(frame, operation);
        if (result != ESP_OK) return result;
        if (type_is(frame, "ready")) {
            const bool accepted = accept_ready(frame);
            cJSON_Delete(frame);
            frame = nullptr;
            if (!accepted || epoch.load() != request_epoch) return ESP_ERR_INVALID_STATE;
            continue;
        }
        double frame_id;
        if (!number_field(frame, "id", 0, UINT32_MAX, frame_id)) {
            cJSON_Delete(frame);
            frame = nullptr;
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (static_cast<uint32_t>(frame_id) == id) return ESP_OK;
        cJSON_Delete(frame);
        frame = nullptr;
    }
}

esp_err_t send_body(uint32_t id, const uint8_t* body, size_t length,
                    const Operation& operation) {
    for (size_t offset = 0; offset < length;) {
        const size_t count = std::min(kChunkBytes, length - offset);
        char encoded[((kChunkBytes + 2) / 3) * 4 + 1];
        size_t encoded_length = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded), sizeof(encoded),
                                  &encoded_length, body + offset, count) != 0) return ESP_FAIL;
        encoded[encoded_length] = '\0';
        char line[kLineBytes];
        const int line_length = std::snprintf(line, sizeof(line),
            "{\"type\":\"data\",\"id\":%" PRIu32 ",\"chunk\":\"%s\"}", id, encoded);
        const esp_err_t result = write_line(line, static_cast<size_t>(line_length), operation);
        if (result != ESP_OK) return result;
        offset += count;
    }
    char line[64];
    const int count = std::snprintf(line, sizeof(line), "{\"type\":\"end\",\"id\":%" PRIu32 "}", id);
    return write_line(line, static_cast<size_t>(count), operation);
}

esp_err_t receive_response(uint32_t id, uint32_t request_epoch, std::string& response,
                           int& status, size_t limit, const Operation& operation) {
    cJSON* frame = nullptr;
    esp_err_t result = read_request_frame(id, request_epoch, frame, operation);
    if (result != ESP_OK) return result;
    double response_status, declared;
    const bool valid = type_is(frame, "response") &&
        number_field(frame, "status", 100, 599, response_status) &&
        number_field(frame, "length", 0, limit, declared) &&
        valid_text(string_field(frame, "contentType"), 128);
    cJSON_Delete(frame);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;
    const size_t expected = static_cast<size_t>(declared);
    while (true) {
        result = read_request_frame(id, request_epoch, frame, operation);
        if (result != ESP_OK) return result;
        if (type_is(frame, "end")) {
            cJSON_Delete(frame);
            if (response.size() != expected) return ESP_ERR_INVALID_SIZE;
            status = static_cast<int>(response_status);
            return ESP_OK;
        }
        const char* chunk = string_field(frame, "chunk");
        if (!type_is(frame, "data") || chunk == nullptr ||
            std::strlen(chunk) > ((kChunkBytes + 2) / 3) * 4) {
            cJSON_Delete(frame);
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint8_t decoded[kChunkBytes];
        size_t decoded_length = 0;
        const int decoded_result = mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_length,
            reinterpret_cast<const unsigned char*>(chunk), std::strlen(chunk));
        // Round-trip validation also rejects whitespace and noncanonical padding.
        char canonical[((kChunkBytes + 2) / 3) * 4 + 1];
        size_t canonical_length = 0;
        const bool good_chunk = decoded_result == 0 && decoded_length > 0 &&
            decoded_length <= expected - response.size() &&
            mbedtls_base64_encode(reinterpret_cast<unsigned char*>(canonical), sizeof(canonical),
                                  &canonical_length, decoded, decoded_length) == 0 &&
            canonical_length == std::strlen(chunk) &&
            std::memcmp(canonical, chunk, canonical_length) == 0;
        cJSON_Delete(frame);
        if (!good_chunk) return ESP_ERR_INVALID_RESPONSE;
        response.append(reinterpret_cast<const char*>(decoded), decoded_length);
    }
}

void hello_task(void*) {
    int64_t next_hello = 0;
    for (;;) {
        const bool should_hello = enabled.load() && esp_timer_get_time() >= next_hello;
        if ((should_hello || pending_bye.load()) && xSemaphoreTake(request_lock, 0) == pdTRUE) {
            send_pending_bye();
            if (enabled.load() && should_hello && cable_connected()) {
                const Operation operation{esp_timer_get_time() + kHandshakeUs, cancellation.load()};
                (void)handshake(operation);
                next_hello = esp_timer_get_time() + kHelloIntervalUs;
            }
            xSemaphoreGive(request_lock);
        }
        if (!enabled.load()) next_hello = 0;
        // Notifications wake this task immediately on foreground/bye changes;
        // while inactive it performs no reads, writes or heartbeat traffic.
        const int delay_ms = pending_bye.load() || enabled.load() ? 20 : 3000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}

}  // namespace

bool is_direct_url(const std::string& url) {
    return url == kBaseUrl || url == std::string(kBaseUrl) + "/";
}

esp_err_t initialize() {
    while (!initialized.load()) {
        bool expected = false;
        if (!initializing.compare_exchange_strong(expected, true)) {
            vTaskDelay(1);
            continue;
        }
        // Another initializer may have finished between the loop condition
        // and our compare_exchange, so never install the driver twice.
        if (initialized.load()) {
            initializing.store(false);
            return ESP_OK;
        }
        request_lock = xSemaphoreCreateMutex();
        esp_err_t result = request_lock == nullptr ? ESP_ERR_NO_MEM : ESP_OK;
        uint8_t mac[6] = {};
        if (result == ESP_OK) result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (result == ESP_OK) {
            std::snprintf(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            usb_serial_jtag_driver_config_t config = {};
            config.rx_buffer_size = 4096;
            config.tx_buffer_size = 2048;
            result = usb_serial_jtag_driver_install(&config);
        }
        if (result == ESP_OK) {
            if (xTaskCreate(hello_task, "vibe_usb", 8192, nullptr, 3, &hello_task_handle) != pdPASS) {
                usb_serial_jtag_driver_uninstall();
                result = ESP_ERR_NO_MEM;
            }
        }
        if (result != ESP_OK) {
            if (request_lock != nullptr) vSemaphoreDelete(request_lock);
            request_lock = nullptr;
            initializing.store(false);
            return result;
        }
        initialized.store(true);
        initializing.store(false);
    }
    return ESP_OK;
}

void set_active(bool value) {
    if (value && initialize() != ESP_OK) return;
    const bool previous = enabled.exchange(value);
    if (!value && previous) {
        revoke_authorization();
        pending_bye.store(true);
        cancellation.fetch_add(1);
        invalidate_connection();
    }
    if (hello_task_handle != nullptr) xTaskNotifyGive(hello_task_handle);
}

bool active() { return enabled.load(); }

bool connected() {
    return initialized.load() && enabled.load() && cable_connected() && ready.load();
}

uint32_t connection_epoch() {
    if (initialized.load()) (void)cable_connected();
    return epoch.load();
}

bool authorize_connection(uint32_t expected_epoch, const std::string& authorization) {
    if (authorization.empty() || authorization.find('\0') != std::string::npos ||
        !valid_text(authorization.c_str(), 128) || !connected() ||
        epoch.load() != expected_epoch || expected_epoch == UINT32_MAX) return false;
    portENTER_CRITICAL(&authorization_lock);
    std::memcpy(authorized_bearer, authorization.c_str(), authorization.size() + 1);
    authorized_epoch.store(expected_epoch);
    portEXIT_CRITICAL(&authorization_lock);
    // A concurrent foreground exit/disconnect must not leave an enabled gate.
    if (!connected() || epoch.load() != expected_epoch) {
        revoke_authorization();
        return false;
    }
    return true;
}

void revoke_authorization() {
    portENTER_CRITICAL(&authorization_lock);
    authorized_epoch.store(UINT32_MAX);
    std::memset(authorized_bearer, 0, sizeof(authorized_bearer));
    portEXIT_CRITICAL(&authorization_lock);
}

bool authorization_matches(const std::string& authorization, uint32_t expected_epoch) {
    portENTER_CRITICAL(&authorization_lock);
    const bool matches = authorized_epoch.load() == expected_epoch &&
        std::strcmp(authorized_bearer, authorization.c_str()) == 0;
    portEXIT_CRITICAL(&authorization_lock);
    return matches;
}

void cancel_current() { cancellation.fetch_add(1); }

esp_err_t request(const std::string& path, bool post, const std::string& authorization,
                  const char* content_type, const uint8_t* body, size_t length,
                  std::string& response, int& status, size_t response_limit, int timeout_ms,
                  const std::atomic<bool>* cancellation_flag) {
    response.clear();
    status = 0;
    if (path.size() > 512 || path.find('\0') != std::string::npos ||
        path.find('#') != std::string::npos || !valid_text(path.c_str(), 512) ||
        authorization.find('\0') != std::string::npos || !valid_text(authorization.c_str(), 128) ||
        !valid_text(content_type == nullptr ? "" : content_type, 128) || length > kMaxRequest ||
        (length > 0 && body == nullptr) || response_limit > kMaxResponse || timeout_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized.load() || !enabled.load()) return ESP_ERR_INVALID_STATE;
    const Operation operation{esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000,
                              cancellation.load(), cancellation_flag};
    // Include the lock wait in the deadline and make it cancellable too.
    for (;;) {
        const esp_err_t error = operation_error(operation);
        if (error != ESP_OK) return error;
        if (xSemaphoreTake(request_lock, wait_ticks(operation.deadline)) == pdTRUE) break;
    }
    const uint32_t before_handshake = epoch.load();
    const bool known_connection = !connection_id.empty();
    send_pending_bye();
    const Operation handshake_operation{std::min(operation.deadline,
        esp_timer_get_time() + kHandshakeUs), operation.cancellation_generation, cancellation_flag};
    esp_err_t result = !authorization.empty() && !authorization_matches(authorization, before_handshake)
        ? ESP_ERR_INVALID_STATE : handshake(handshake_operation);
    uint32_t id = 0;
    bool started = false;
    if (result == ESP_OK && !authorization.empty() &&
        (!known_connection || before_handshake != epoch.load() ||
         !authorization_matches(authorization, epoch.load()))) result = ESP_ERR_INVALID_STATE;
    const uint32_t request_epoch = epoch.load();
    if (result == ESP_OK) {
        id = next_id++;
        cJSON* frame = cJSON_CreateObject();
        if (frame == nullptr) {
            result = ESP_ERR_NO_MEM;
        } else {
            bool built = cJSON_AddStringToObject(frame, "type", "request") != nullptr &&
                cJSON_AddNumberToObject(frame, "id", id) != nullptr &&
                cJSON_AddStringToObject(frame, "method", post ? "POST" : "GET") != nullptr &&
                cJSON_AddStringToObject(frame, "path", path.c_str()) != nullptr &&
                cJSON_AddStringToObject(frame, "authorization", authorization.c_str()) != nullptr &&
                cJSON_AddStringToObject(frame, "contentType", content_type == nullptr ? "" : content_type) != nullptr &&
                cJSON_AddNumberToObject(frame, "length", length) != nullptr;
            if (built) {
                started = true;
                result = write_json(frame, operation);
            } else {
                cJSON_Delete(frame);
                result = ESP_ERR_NO_MEM;
            }
        }
    }
    if (result == ESP_OK) result = send_body(id, body, length, operation);
    if (result == ESP_OK) result = receive_response(id, request_epoch, response, status, response_limit, operation);
    if (result == ESP_OK) result = operation_error(operation);
    if (result != ESP_OK) {
        if (started) send_control(id, false);
        response.clear();
        status = 0;
    }
    send_pending_bye();
    xSemaphoreGive(request_lock);
    return result;
}

}  // namespace vibe_usb
