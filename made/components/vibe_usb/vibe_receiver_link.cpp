#include "vibe_usb.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>
#include <lwip/sockets.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

namespace vibe_usb {
namespace {

constexpr size_t kChunkBytes = 1024;
constexpr size_t kLineBytes = 2048;
constexpr size_t kMaxRequest = 1100000;
constexpr size_t kMaxResponse = 256 * 1024;
constexpr uint16_t kPort = 8788;
constexpr int kIoSliceMs = 200;

SemaphoreHandle_t link_lock = nullptr;

struct Deadline {
    int64_t at;
    const std::atomic<bool>* cancel = nullptr;
};

bool expired(const Deadline& deadline) {
    return esp_timer_get_time() >= deadline.at ||
           (deadline.cancel != nullptr && deadline.cancel->load());
}

bool valid_text(const char* value, size_t maximum) {
    if (value == nullptr || std::strlen(value) > maximum) return false;
    for (const char* c = value; *c; ++c) {
        if (static_cast<unsigned char>(*c) < 0x20 || static_cast<unsigned char>(*c) > 0x7e) return false;
    }
    return true;
}

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) close(fd_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

bool send_all(int fd, const char* data, size_t length, const Deadline& deadline) {
    size_t sent = 0;
    while (sent < length) {
        if (expired(deadline)) return false;
        const int wrote = send(fd, data + sent, length - sent, 0);
        if (wrote > 0) {
            sent += static_cast<size_t>(wrote);
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;
    }
    return true;
}

bool send_line(int fd, const char* json, size_t length, const Deadline& deadline) {
    return length + 1 <= kLineBytes && send_all(fd, json, length, deadline) &&
           send_all(fd, "\n", 1, deadline);
}

bool read_line(int fd, char* line, size_t capacity, const Deadline& deadline) {
    size_t length = 0;
    while (!expired(deadline)) {
        char value = 0;
        const int got = recv(fd, &value, 1, 0);
        if (got == 0) return false;
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (value == '\n') {
            if (length > 0 && line[length - 1] == '\r') --length;
            if (length == 0) continue;
            line[length] = '\0';
            return true;
        }
        if (value == '\0' || length + 1 >= capacity) return false;
        line[length++] = value;
    }
    return false;
}

const char* string_field(const cJSON* frame, const char* name) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(frame, name);
    return cJSON_IsString(value) ? cJSON_GetStringValue(value) : nullptr;
}

bool type_is(const cJSON* frame, const char* type) {
    const char* actual = string_field(frame, "type");
    return actual != nullptr && std::strcmp(actual, type) == 0;
}

bool number_field(const cJSON* frame, const char* name, double minimum, double maximum, double& value) {
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(frame, name);
    if (!cJSON_IsNumber(field)) return false;
    value = cJSON_GetNumberValue(field);
    return std::isfinite(value) && value >= minimum && value <= maximum && std::floor(value) == value;
}

cJSON* read_frame(int fd, const Deadline& deadline) {
    char line[kLineBytes];
    if (!read_line(fd, line, sizeof(line), deadline)) return nullptr;
    return cJSON_ParseWithLength(line, std::strlen(line));
}

bool connect_receiver(int fd, const Deadline& deadline) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl((192u << 24) | (168u << 16) | (4u << 8) | 1u);
    const int started = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (started < 0 && errno != EINPROGRESS) return false;
    if (started < 0) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        const int64_t remaining_us = deadline.at - esp_timer_get_time();
        if (remaining_us <= 0) return false;
        timeval wait {};
        wait.tv_sec = static_cast<time_t>(remaining_us / 1000000);
        wait.tv_usec = static_cast<suseconds_t>(remaining_us % 1000000);
        if (select(fd + 1, nullptr, &writable, nullptr, &wait) <= 0) return false;
        int so_error = 0;
        socklen_t length = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) < 0 || so_error != 0) return false;
    }
    if (fcntl(fd, F_SETFL, flags) < 0) return false;
    const timeval slice { .tv_sec = 0, .tv_usec = kIoSliceMs * 1000 };
    const int nodelay = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &slice, sizeof(slice));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &slice, sizeof(slice));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    return true;
}

bool send_body(int fd, uint32_t id, const uint8_t* body, size_t length, const Deadline& deadline) {
    for (size_t offset = 0; offset < length;) {
        const size_t count = std::min(kChunkBytes, length - offset);
        char encoded[((kChunkBytes + 2) / 3) * 4 + 1];
        size_t encoded_length = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded), sizeof(encoded),
                                  &encoded_length, body + offset, count) != 0) return false;
        encoded[encoded_length] = '\0';
        char line[kLineBytes];
        const int line_length = std::snprintf(line, sizeof(line),
            "{\"type\":\"data\",\"id\":%" PRIu32 ",\"chunk\":\"%s\"}", id, encoded);
        if (line_length <= 0 || !send_line(fd, line, static_cast<size_t>(line_length), deadline)) return false;
        offset += count;
    }
    char line[64];
    const int count = std::snprintf(line, sizeof(line), "{\"type\":\"end\",\"id\":%" PRIu32 "}", id);
    return count > 0 && send_line(fd, line, static_cast<size_t>(count), deadline);
}

bool receive_response(int fd, uint32_t id, std::string& response, int& status, size_t limit,
                      const Deadline& deadline) {
    cJSON* frame = nullptr;
    while (!expired(deadline)) {
        frame = read_frame(fd, deadline);
        if (frame == nullptr) return false;
        if (type_is(frame, "ready") || type_is(frame, "hello")) {
            cJSON_Delete(frame);
            continue;
        }
        break;
    }
    double response_status = 0;
    double declared = -1;
    const bool header_ok = frame != nullptr && type_is(frame, "response") &&
        number_field(frame, "status", 100, 599, response_status) &&
        number_field(frame, "length", 0, limit, declared) &&
        valid_text(string_field(frame, "contentType"), 128);
    cJSON_Delete(frame);
    if (!header_ok) return false;
    const size_t expected = static_cast<size_t>(declared);
    while (!expired(deadline)) {
        frame = read_frame(fd, deadline);
        if (frame == nullptr) return false;
        if (type_is(frame, "ready") || type_is(frame, "hello")) {
            cJSON_Delete(frame);
            continue;
        }
        if (type_is(frame, "end")) {
            cJSON_Delete(frame);
            if (response.size() != expected) return false;
            status = static_cast<int>(response_status);
            return true;
        }
        const char* chunk = string_field(frame, "chunk");
        if (!type_is(frame, "data") || chunk == nullptr ||
            std::strlen(chunk) > ((kChunkBytes + 2) / 3) * 4) {
            cJSON_Delete(frame);
            return false;
        }
        uint8_t decoded[kChunkBytes];
        size_t decoded_length = 0;
        const int decoded_result = mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_length,
            reinterpret_cast<const unsigned char*>(chunk), std::strlen(chunk));
        char canonical[((kChunkBytes + 2) / 3) * 4 + 1];
        size_t canonical_length = 0;
        const bool good_chunk = decoded_result == 0 && decoded_length > 0 &&
            response.size() <= expected && decoded_length <= expected - response.size() &&
            mbedtls_base64_encode(reinterpret_cast<unsigned char*>(canonical), sizeof(canonical),
                                  &canonical_length, decoded, decoded_length) == 0 &&
            canonical_length == std::strlen(chunk) &&
            std::memcmp(canonical, chunk, canonical_length) == 0;
        cJSON_Delete(frame);
        if (!good_chunk) return false;
        response.append(reinterpret_cast<const char*>(decoded), decoded_length);
        char ack[48];
        const int ack_length = std::snprintf(ack, sizeof(ack), "{\"type\":\"ack\",\"id\":%" PRIu32 "}", id);
        if (ack_length <= 0 || !send_line(fd, ack, static_cast<size_t>(ack_length), deadline)) return false;
    }
    return false;
}

}  // namespace

bool is_receiver_url(const std::string& url) {
    const std::string base(kReceiverUrl);
    return url == base || url == base + "/" || url.rfind(base + "/", 0) == 0;
}

esp_err_t receiver_request(const std::string& path, bool post, const std::string& authorization,
                           const char* content_type, const uint8_t* body, size_t length,
                           std::string& response, int& status, size_t response_limit, int timeout_ms,
                           const std::atomic<bool>* cancellation_flag) {
    response.clear();
    status = 0;
    if (path.size() > 512 || path.find('\0') != std::string::npos || path.find('#') != std::string::npos ||
        !valid_text(path.c_str(), 512) || authorization.find('\0') != std::string::npos ||
        !valid_text(authorization.c_str(), 128) ||
        !valid_text(content_type == nullptr ? "" : content_type, 128) || length > kMaxRequest ||
        (length > 0 && body == nullptr) || response_limit > kMaxResponse || timeout_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (link_lock == nullptr) {
        link_lock = xSemaphoreCreateMutex();
        if (link_lock == nullptr) return ESP_ERR_NO_MEM;
    }
    const Deadline deadline{esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000, cancellation_flag};
    if (xSemaphoreTake(link_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_FAIL;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd >= 0) {
        Socket socket(fd);
        bool started = false;
        if (connect_receiver(fd, deadline)) {
            const uint32_t id = 1;
            cJSON* frame = cJSON_CreateObject();
            char* json = nullptr;
            if (frame != nullptr) {
                const bool built = cJSON_AddStringToObject(frame, "type", "request") != nullptr &&
                    cJSON_AddNumberToObject(frame, "id", id) != nullptr &&
                    cJSON_AddStringToObject(frame, "method", post ? "POST" : "GET") != nullptr &&
                    cJSON_AddStringToObject(frame, "path", path.c_str()) != nullptr &&
                    cJSON_AddStringToObject(frame, "authorization", authorization.c_str()) != nullptr &&
                    cJSON_AddStringToObject(frame, "contentType",
                                            content_type == nullptr ? "" : content_type) != nullptr &&
                    cJSON_AddNumberToObject(frame, "length", length) != nullptr;
                if (built) json = cJSON_PrintUnformatted(frame);
                cJSON_Delete(frame);
            }
            if (json != nullptr && std::strlen(json) + 1 <= kLineBytes) {
                started = true;
                const bool header_sent = send_line(fd, json, std::strlen(json), deadline);
                cJSON_free(json);
                json = nullptr;
                if (header_sent && send_body(fd, id, body, length, deadline) &&
                    receive_response(fd, id, response, status, response_limit, deadline) &&
                    !expired(deadline)) {
                    result = ESP_OK;
                } else if (expired(deadline)) {
                    result = ESP_ERR_TIMEOUT;
                }
            }
            if (json != nullptr) cJSON_free(json);
            if (result != ESP_OK && started) {
                // Finish a partial line, then tell the computer this request stopped.
                const Deadline grace{esp_timer_get_time() + 1000000, nullptr};
                char cancel[64];
                const int cancel_length = std::snprintf(cancel, sizeof(cancel),
                    "\n{\"type\":\"cancel\",\"id\":%" PRIu32 "}\n", id);
                if (cancel_length > 0) {
                    (void)send_all(fd, cancel, static_cast<size_t>(cancel_length), grace);
                }
            }
        }
    }
    if (result != ESP_OK) {
        response.clear();
        status = 0;
    }
    xSemaphoreGive(link_lock);
    return result;
}

}  // namespace vibe_usb
