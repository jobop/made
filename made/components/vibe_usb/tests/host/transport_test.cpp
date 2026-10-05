// Compiles the production transport against a deterministic USB/FreeRTOS peer.
// cJSON is the actual IDF dependency; base64 implements the platform API below.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "fake_idf.hpp"
#include "cJSON.h"
#include "../../vibe_usb.cpp"

namespace fake {
int64_t now = 1000;
bool cable = true;
bool acknowledge = true;
bool answer = true;
bool overlong = false;
bool wrong_length = false;
bool stale_first = false;
bool change_during_response = false;
std::string connection = "00112233445566778899aabbccddeeff";
std::string outgoing;
std::deque<char> incoming;
std::vector<std::string> types;
std::vector<std::string> authorizations;
std::vector<size_t> chunks;
std::string response_body = "{\"ok\":true}";
std::string uploaded_body;
size_t declared_upload = 0;
uint32_t current_id = 0;
std::function<void(const std::string&)> on_frame;
std::function<void()> on_empty_read;

void enqueue(const std::string& text) {
    incoming.insert(incoming.end(), text.begin(), text.end());
}

std::string encoded(const std::string& bytes) {
    std::string result(((bytes.size() + 2) / 3) * 4 + 1, '\0');
    size_t length = 0;
    assert(mbedtls_base64_encode(reinterpret_cast<unsigned char*>(result.data()), result.size(),
        &length, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) == 0);
    result.resize(length);
    return result;
}

std::string id_json(uint32_t id) { return "\"id\":" + std::to_string(id); }

void hello() {
    if (acknowledge) enqueue("{\"type\":\"ready\",\"version\":1,\"connectionId\":\"" + connection + "\"}\n");
}

void response(uint32_t id) {
    if (overlong) { enqueue(std::string(2048, 'x') + "\n"); return; }
    if (stale_first) enqueue("{\"type\":\"end\"," + id_json(id - 1) + "}\n");
    if (change_during_response) {
        connection = "aabbccddeeff00112233445566778899";
        hello();
    }
    enqueue("{\"type\":\"response\"," + id_json(id) + ",\"status\":201,\"contentType\":\"application/json\",\"length\":" +
        std::to_string(response_body.size() + (wrong_length ? 1 : 0)) + "}\n");
    for (size_t offset = 0; offset < response_body.size(); offset += 1024) {
        enqueue("{\"type\":\"data\"," + id_json(id) + ",\"chunk\":\"" +
            encoded(response_body.substr(offset, 1024)) + "\"}\n");
    }
    enqueue("{\"type\":\"end\"," + id_json(id) + "}\n");
}

void line(const std::string& text) {
    if (text.empty()) return;
    assert(text.size() + 1 <= 2048);
    cJSON* frame = cJSON_Parse(text.c_str());
    assert(frame);
    const char* type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "type"));
    assert(type);
    const std::string kind = type;
    types.push_back(kind);
    if (kind == "hello") {
        assert(std::strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "mode")), "direct") == 0);
        assert(std::strlen(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "deviceId"))) == 12);
        assert(!cJSON_GetObjectItemCaseSensitive(frame, "ssid"));
        assert(!cJSON_GetObjectItemCaseSensitive(frame, "password"));
        hello();
    } else if (kind == "request") {
        current_id = static_cast<uint32_t>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(frame, "id")));
        authorizations.emplace_back(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "authorization")));
        declared_upload = static_cast<size_t>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(frame, "length")));
        uploaded_body.clear();
    } else if (kind == "data") {
        const char* chunk = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "chunk"));
        unsigned char bytes[1024];
        size_t count = 0;
        assert(mbedtls_base64_decode(bytes, sizeof(bytes), &count,
            reinterpret_cast<const unsigned char*>(chunk), std::strlen(chunk)) == 0);
        assert(count > 0 && count <= 1024);
        chunks.push_back(count);
        uploaded_body.append(reinterpret_cast<char*>(bytes), count);
    } else if (kind == "end") {
        assert(uploaded_body.size() == declared_upload);
        if (answer) response(current_id);
    }
    cJSON_Delete(frame);
    if (on_frame) on_frame(kind);
}

void reset_peer() {
    cable = true;
    acknowledge = true;
    answer = true;
    overlong = wrong_length = stale_first = change_during_response = false;
    incoming.clear();
    outgoing.clear();
    types.clear();
    authorizations.clear();
    chunks.clear();
    response_body = "{\"ok\":true}";
    on_frame = nullptr;
    on_empty_read = nullptr;
}

int count(const std::string& type) { return std::count(types.begin(), types.end(), type); }
}  // namespace fake

int64_t esp_timer_get_time() { return fake::now; }
esp_err_t esp_read_mac(uint8_t* mac, int) {
    const uint8_t value[6] = {0x30, 0x3a, 0x12, 0x34, 0x56, 0x78};
    std::memcpy(mac, value, sizeof(value));
    return ESP_OK;
}
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t*) { return ESP_OK; }
void usb_serial_jtag_driver_uninstall() {}
bool usb_serial_jtag_is_connected() { return fake::cable; }
int usb_serial_jtag_read_bytes(void* target, size_t capacity, TickType_t ticks) {
    if (fake::incoming.empty()) {
        fake::now += static_cast<int64_t>(ticks) * 1000;
        if (ticks > 0 && fake::on_empty_read) fake::on_empty_read();
        return 0;
    }
    // Fragment incoming JSON at unrelated positions to exercise serial buffering.
    const size_t length = std::min({capacity, fake::incoming.size(), size_t{37}});
    for (size_t index = 0; index < length; ++index) {
        static_cast<char*>(target)[index] = fake::incoming.front();
        fake::incoming.pop_front();
    }
    return static_cast<int>(length);
}
int usb_serial_jtag_write_bytes(const void* source, size_t length, TickType_t) {
    // Partial writes exercise serial_write_all and request cancellation slices.
    length = std::min(length, size_t{79});
    fake::outgoing.append(static_cast<const char*>(source), length);
    for (;;) {
        const size_t newline = fake::outgoing.find('\n');
        if (newline == std::string::npos) break;
        const std::string line = fake::outgoing.substr(0, newline);
        fake::outgoing.erase(0, newline + 1);
        fake::line(line);
    }
    return static_cast<int>(length);
}
SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::timed_mutex; }
void vSemaphoreDelete(SemaphoreHandle_t value) { delete static_cast<std::timed_mutex*>(value); }
int xSemaphoreTake(SemaphoreHandle_t value, TickType_t ticks) {
    if (static_cast<std::timed_mutex*>(value)->try_lock()) return pdTRUE;
    fake::now += static_cast<int64_t>(ticks) * 1000;
    return 0;
}
int xSemaphoreGive(SemaphoreHandle_t value) { static_cast<std::timed_mutex*>(value)->unlock(); return pdTRUE; }
int xTaskCreate(void(*)(void*), const char*, uint32_t, void*, int, TaskHandle_t* task) {
    *task = reinterpret_cast<TaskHandle_t>(1);
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { fake::now += static_cast<int64_t>(ticks) * 1000; }
void xTaskNotifyGive(TaskHandle_t) {}
uint32_t ulTaskNotifyTake(int, TickType_t ticks) { vTaskDelay(ticks); return 0; }

constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int mbedtls_base64_encode(unsigned char* out, size_t capacity, size_t* used,
                           const unsigned char* bytes, size_t length) {
    *used = ((length + 2) / 3) * 4;
    if (capacity <= *used) return -1;
    size_t output = 0;
    for (size_t offset = 0; offset < length; offset += 3) {
        const uint32_t value = (bytes[offset] << 16) |
            (offset + 1 < length ? bytes[offset + 1] << 8 : 0) |
            (offset + 2 < length ? bytes[offset + 2] : 0);
        out[output++] = alphabet[(value >> 18) & 63];
        out[output++] = alphabet[(value >> 12) & 63];
        out[output++] = offset + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        out[output++] = offset + 2 < length ? alphabet[value & 63] : '=';
    }
    out[output] = 0;
    return 0;
}
int mbedtls_base64_decode(unsigned char* out, size_t capacity, size_t* used,
                           const unsigned char* bytes, size_t length) {
    *used = 0;
    if (length % 4) return -1;
    for (size_t offset = 0; offset < length; offset += 4) {
        uint32_t value = 0;
        size_t padding = 0;
        for (size_t index = 0; index < 4; ++index) {
            value <<= 6;
            if (bytes[offset + index] == '=') { ++padding; continue; }
            const char* found = std::strchr(alphabet, bytes[offset + index]);
            if (!found || padding) return -1;
            value |= static_cast<uint32_t>(found - alphabet);
        }
        if (padding > 2 || *used + 3 - padding > capacity) return -1;
        for (size_t index = 0; index < 3 - padding; ++index) {
            out[(*used)++] = static_cast<unsigned char>(value >> (16 - 8 * index));
        }
    }
    return 0;
}

esp_err_t call(std::string& response, int& status, const std::string& authorization = "",
               const std::string& body = "", size_t limit = 8192, int timeout = 30000,
               const std::atomic<bool>* cancelled = nullptr) {
    return vibe_usb::request(authorization.empty() ? "/pair/start" : "/device/voice", true,
        authorization, "application/json", reinterpret_cast<const uint8_t*>(body.data()),
        body.size(), response, status, limit, timeout, cancelled);
}

int main() {
    assert(fake::encoded("M") == "TQ==" && fake::encoded("Ma") == "TWE=" && fake::encoded("Man") == "TWFu");
    std::string response;
    int status = 0;
    assert(vibe_usb::initialize() == ESP_OK);
    assert(call(response, status) == ESP_ERR_INVALID_STATE);
    assert(fake::types.empty());
    vibe_usb::set_active(true);
    assert(call(response, status) == ESP_OK && status == 201 && response == fake::response_body);
    assert(vibe_usb::connected());
    assert(fake::count("hello") == 1);

    fake::reset_peer();
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_STATE);
    assert(fake::authorizations.empty());
    assert(vibe_usb::authorize_connection(vibe_usb::connection_epoch(), "Bearer secret"));

    // Maximum allowed response, exact payload bounds, and stale request IDs.
    fake::reset_peer();
    fake::response_body.assign(256 * 1024, 'R');
    fake::stale_first = true;
    const std::string upload(1024 * 3 + 7, 'U');
    assert(call(response, status, "Bearer secret", upload, 256 * 1024) == ESP_OK);
    assert(response == fake::response_body && fake::uploaded_body == upload);
    assert((fake::chunks == std::vector<size_t>{1024, 1024, 1024, 7}));

    // A desktop change during the mandatory ready handshake emits no token.
    fake::reset_peer();
    const uint32_t old_epoch = vibe_usb::connection_epoch();
    fake::connection = "ffeeddccbbaa99887766554433221100";
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_STATE);
    assert(fake::authorizations.empty() && vibe_usb::connection_epoch() != old_epoch);
    assert(!vibe_usb::authorize_connection(old_epoch, "Bearer secret"));
    assert(call(response, status) == ESP_OK);  // Pairing is still allowed.
    assert(vibe_usb::authorize_connection(vibe_usb::connection_epoch(), "Bearer secret"));

    // A suspended worker carrying the old computer's token cannot borrow the
    // new computer's authorization gate after another task completes pairing.
    fake::reset_peer();
    assert(vibe_usb::authorize_connection(vibe_usb::connection_epoch(), "Bearer new-computer-token"));
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_STATE);
    assert(fake::authorizations.empty() && fake::types.empty());
    assert(call(response, status, "Bearer new-computer-token") == ESP_OK);
    assert(vibe_usb::authorize_connection(vibe_usb::connection_epoch(), "Bearer secret"));

    // A change learned by idle discovery is checked before the next handshake.
    fake::reset_peer();
    fake::connection = "11223344556677889900aabbccddeeff";
    assert(vibe_usb::handshake({fake::now + 1000000, vibe_usb::cancellation.load()}) == ESP_OK);
    fake::reset_peer();
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_STATE);
    assert(fake::authorizations.empty() && fake::types.empty());
    assert(vibe_usb::authorize_connection(vibe_usb::connection_epoch(), "Bearer secret"));

    fake::reset_peer();
    fake::wrong_length = true;
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_SIZE);
    assert(response.empty() && status == 0 && fake::count("cancel") == 1);
    fake::reset_peer();
    fake::response_body.assign(8193, 'X');
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_RESPONSE);
    assert(response.empty() && fake::count("cancel") == 1);
    fake::reset_peer();
    fake::overlong = true;
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_SIZE);
    assert(response.empty() && fake::count("cancel") == 1);

    // Cancel during upload prevents any more chunks and does not poison retry.
    fake::reset_peer();
    fake::on_frame = [](const std::string& type) { if (type == "data") vibe_usb::cancel_current(); };
    assert(call(response, status, "Bearer secret", upload) == ESP_ERR_INVALID_STATE);
    assert(fake::chunks.size() == 1 && fake::count("cancel") == 1 && fake::count("end") == 0);
    fake::reset_peer();
    assert(call(response, status, "Bearer secret") == ESP_OK);

    // A cancellation preceding request() is preserved by the external flag.
    fake::reset_peer();
    std::atomic<bool> cancelled{true};
    assert(call(response, status, "Bearer secret", "", 8192, 30000, &cancelled) == ESP_ERR_INVALID_STATE);
    assert(fake::types.empty());

    // Exit while the desktop transcribes interrupts in at most one I/O slice,
    // sends cancel and bye, and keeps the next foreground session usable.
    fake::reset_peer();
    fake::answer = false;
    fake::on_empty_read = [] { vibe_usb::set_active(false); };
    const int64_t before_exit = fake::now;
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_STATE);
    assert(fake::now - before_exit <= 20000);
    assert(fake::count("cancel") == 1 && fake::count("bye") == 1);
    assert(!vibe_usb::active() && !vibe_usb::connected());
    fake::reset_peer();
    vibe_usb::set_active(true);
    assert(call(response, status) == ESP_OK);
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_STATE);
    assert(vibe_usb::authorize_connection(vibe_usb::connection_epoch(), "Bearer secret"));

    fake::reset_peer();
    fake::answer = false;
    const int64_t before_timeout = fake::now;
    assert(call(response, status, "Bearer secret", "", 8192, 60) == ESP_ERR_TIMEOUT);
    assert(fake::now - before_timeout == 60000 && fake::count("cancel") == 1);
    fake::reset_peer();
    assert(call(response, status, "Bearer secret") == ESP_OK);

    // Physical disconnect invalidates authorization, including reconnect to the
    // same process/connection ID; stale proof cannot silently regain permission.
    fake::reset_peer();
    const uint32_t before_disconnect = vibe_usb::connection_epoch();
    fake::cable = false;
    assert(!vibe_usb::connected() && vibe_usb::connection_epoch() != before_disconnect);
    fake::cable = true;
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_STATE);
    assert(call(response, status) == ESP_OK);
    assert(vibe_usb::authorize_connection(vibe_usb::connection_epoch(), "Bearer secret"));
    fake::reset_peer();
    fake::change_during_response = true;
    assert(call(response, status, "Bearer secret") == ESP_ERR_INVALID_STATE);
    assert(response.empty() && fake::count("cancel") == 1);

    std::puts("vibe_usb: protocol bounds, epoch authorization, cancellation and retry tests passed");
}
