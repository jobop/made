#include "weather_data.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"

namespace esp_brookesia::apps {
namespace {
constexpr char kTag[] = "Weather";
constexpr char kNvsNamespace[] = "weather_app";
constexpr size_t kMaxResponseBytes = 4096;
constexpr uint32_t kRefreshMs = 30 * 60 * 1000;
constexpr uint32_t kRetryMs = 2 * 60 * 1000;
constexpr uint32_t kWifiPollMs = 10 * 1000;

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \r\n\t");
    return text.substr(first, last - first + 1);
}

std::string nvsString(nvs_handle_t handle, const char* key) {
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size < 1 || size > 96) return {};
    std::string value(size, '\0');
    if (nvs_get_str(handle, key, value.data(), &size) != ESP_OK) return {};
    value.resize(size - 1);
    return value;
}

bool parseCoordinate(const std::string& text, double& value, double limit) {
    if (text.empty()) return false;
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && std::isfinite(value) && std::abs(value) <= limit;
}

std::string number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6f", value);
    return text;
}

std::string urlEncode(const std::string& value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(kHex[ch >> 4]);
            result.push_back(kHex[ch & 15]);
        }
    }
    return result;
}

bool eraseIfPresent(nvs_handle_t handle, const char* key) {
    const esp_err_t error = nvs_erase_key(handle, key);
    return error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND;
}

struct Response {
    std::string body;
    bool too_large = false;
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (event->data_len <= 0) return ESP_OK;
    auto* response = static_cast<Response*>(event->user_data);
    if (response == nullptr || event->data == nullptr ||
        static_cast<size_t>(event->data_len) >= kMaxResponseBytes - response->body.size()) {
        if (response != nullptr) response->too_large = true;
        return ESP_FAIL;
    }
    response->body.append(static_cast<const char*>(event->data), event->data_len);
    return ESP_OK;
}
} // namespace

WeatherData& WeatherData::instance() {
    static WeatherData data;
    return data;
}

WeatherData::WeatherData() : mutex_(xSemaphoreCreateMutex()) {
    configASSERT(mutex_ != nullptr);
}

void WeatherData::start() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (worker_ != nullptr) {
        xSemaphoreGive(mutex_);
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        state_.city = nvsString(handle, "city");
        have_coordinates_ = parseCoordinate(nvsString(handle, "lat"), latitude_, 90) &&
                            parseCoordinate(nvsString(handle, "lon"), longitude_, 180);
        if (!state_.city.empty() && nvsString(handle, "cache_city") == state_.city) {
            int32_t temperature_x10 = 0;
            int32_t code = -1;
            if (nvs_get_i32(handle, "temp_x10", &temperature_x10) == ESP_OK &&
                nvs_get_i32(handle, "code", &code) == ESP_OK &&
                temperature_x10 >= -1000 && temperature_x10 <= 1000 &&
                code >= 0 && code <= 99) {
                state_.temperature_c = temperature_x10 / 10.0f;
                state_.condition = conditionFor(code);
                state_.observed_at = nvsString(handle, "time");
                state_.available = true;
                state_.stale = true;
            }
        }
        nvs_close(handle);
    }

    if (xTaskCreate(taskEntry, "weather_fetch", 8192, this, 3, &worker_) != pdPASS) {
        worker_ = nullptr;
        state_.error = "Weather worker failed";
    }
    xSemaphoreGive(mutex_);
}

bool WeatherData::setCity(const std::string& raw_city) {
    const std::string city = trim(raw_city);
    if (city.empty() || city.size() > 64) return false;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        xSemaphoreGive(mutex_);
        return false;
    }
    const esp_err_t written = nvs_set_str(handle, "city", city.c_str());
    const bool erased = written == ESP_OK && eraseIfPresent(handle, "lat") &&
                        eraseIfPresent(handle, "lon") && eraseIfPresent(handle, "cache_city");
    const esp_err_t committed = erased ? nvs_commit(handle) : ESP_FAIL;
    nvs_close(handle);
    if (committed != ESP_OK) {
        xSemaphoreGive(mutex_);
        return false;
    }

    state_ = WeatherSnapshot{};
    state_.city = city;
    have_coordinates_ = false;
    ++revision_;
    TaskHandle_t worker = worker_;
    xSemaphoreGive(mutex_);
    if (worker != nullptr) xTaskNotifyGive(worker);
    return true;
}

void WeatherData::refresh() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    TaskHandle_t worker = worker_;
    xSemaphoreGive(mutex_);
    if (worker != nullptr) xTaskNotifyGive(worker);
}

WeatherSnapshot WeatherData::snapshot() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    WeatherSnapshot copy = state_;
    xSemaphoreGive(mutex_);
    return copy;
}

void WeatherData::taskEntry(void* context) {
    static_cast<WeatherData*>(context)->run();
}

void WeatherData::run() {
    while (true) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        const std::string city = state_.city;
        const bool have_coordinates = have_coordinates_;
        double lat = latitude_;
        double lon = longitude_;
        const uint32_t revision = revision_;
        xSemaphoreGive(mutex_);

        if (city.empty()) {
            wait(UINT32_MAX);
            continue;
        }

        wifi_ap_record_t access_point = {};
        if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
            recordError(revision, "Waiting for Wi-Fi");
            wait(kWifiPollMs);
            continue;
        }

        if (!have_coordinates) {
            if (!geocode(city, lat, lon)) {
                recordError(revision, "City lookup failed");
                wait(kRetryMs);
                continue;
            }
            xSemaphoreTake(mutex_, portMAX_DELAY);
            const bool changed = revision_ != revision;
            if (!changed) {
                latitude_ = lat;
                longitude_ = lon;
                have_coordinates_ = true;
                nvs_handle_t handle;
                if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
                    nvs_set_str(handle, "lat", number(lat).c_str());
                    nvs_set_str(handle, "lon", number(lon).c_str());
                    nvs_commit(handle);
                    nvs_close(handle);
                }
            }
            xSemaphoreGive(mutex_);
            if (changed) continue;
        }

        WeatherSnapshot result;
        result.city = city;
        int code = -1;
        if (!current(lat, lon, result, code)) {
            recordError(revision, "Weather update failed");
            wait(kRetryMs);
            continue;
        }

        xSemaphoreTake(mutex_, portMAX_DELAY);
        const bool changed = revision_ != revision;
        if (!changed) {
            state_ = result;
            nvs_handle_t handle;
            if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
                nvs_set_str(handle, "cache_city", city.c_str());
                nvs_set_i32(handle, "temp_x10", static_cast<int32_t>(std::lround(result.temperature_c * 10)));
                nvs_set_i32(handle, "code", code);
                nvs_set_str(handle, "time", result.observed_at.c_str());
                nvs_commit(handle);
                nvs_close(handle);
            }
        }
        xSemaphoreGive(mutex_);
        if (!changed) ESP_LOGI(kTag, "%s %.1f C, code %d", city.c_str(), result.temperature_c, code);
        wait(changed ? 0 : kRefreshMs);
    }
}

bool WeatherData::getJson(const std::string& url, std::string& body) {
    Response response;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 6000;
    config.buffer_size = 512;
    config.event_handler = onHttpEvent;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "Accept", "application/json");
    const esp_err_t performed = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (performed != ESP_OK || response.too_large || status != 200 || response.body.empty()) {
        ESP_LOGW(kTag, "HTTPS request failed (%s, HTTP %d)", esp_err_to_name(performed), status);
        return false;
    }
    body.swap(response.body);
    return true;
}

bool WeatherData::geocode(const std::string& city, double& lat, double& lon) {
    const std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
        urlEncode(city) + "&count=1&language=en&format=json";
    std::string body;
    if (!getJson(url, body)) return false;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;
    cJSON* results = cJSON_GetObjectItemCaseSensitive(root, "results");
    cJSON* first = cJSON_GetArrayItem(results, 0);
    cJSON* latitude = cJSON_GetObjectItemCaseSensitive(first, "latitude");
    cJSON* longitude = cJSON_GetObjectItemCaseSensitive(first, "longitude");
    const bool valid = cJSON_IsNumber(latitude) && cJSON_IsNumber(longitude) &&
        std::isfinite(latitude->valuedouble) && std::isfinite(longitude->valuedouble) &&
        std::abs(latitude->valuedouble) <= 90 && std::abs(longitude->valuedouble) <= 180;
    if (valid) {
        lat = latitude->valuedouble;
        lon = longitude->valuedouble;
    }
    cJSON_Delete(root);
    return valid;
}

bool WeatherData::current(double lat, double lon, WeatherSnapshot& value, int& code) {
    const std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + number(lat) +
        "&longitude=" + number(lon) + "&current=temperature_2m,weather_code&timezone=auto";
    std::string body;
    if (!getJson(url, body)) return false;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;
    cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON* temperature = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    cJSON* weather_code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    cJSON* time = cJSON_GetObjectItemCaseSensitive(current, "time");
    const bool valid = cJSON_IsNumber(temperature) && cJSON_IsNumber(weather_code) &&
        cJSON_IsString(time) && time->valuestring != nullptr &&
        std::isfinite(temperature->valuedouble) &&
        temperature->valuedouble >= -100 && temperature->valuedouble <= 100 &&
        weather_code->valuedouble >= 0 && weather_code->valuedouble <= 99 &&
        std::strlen(time->valuestring) <= 32;
    if (valid) {
        value.temperature_c = static_cast<float>(temperature->valuedouble);
        code = weather_code->valueint;
        value.condition = conditionFor(code);
        value.observed_at = time->valuestring;
        value.available = true;
    }
    cJSON_Delete(root);
    return valid;
}

void WeatherData::recordError(uint32_t revision, const char* error) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (revision_ == revision) {
        state_.error = error;
        state_.stale = state_.available;
    }
    xSemaphoreGive(mutex_);
}

void WeatherData::wait(uint32_t milliseconds) {
    const TickType_t ticks = milliseconds == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(milliseconds);
    ulTaskNotifyTake(pdTRUE, ticks);
}

const char* WeatherData::conditionFor(int code) {
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: case 56: case 57: return "Drizzle";
    case 61: case 63: case 65: case 66: case 67: return "Rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: case 82: return "Showers";
    case 85: case 86: return "Snow showers";
    case 95: case 96: case 99: return "Thunderstorm";
    default: return "Unknown";
    }
}

} // namespace esp_brookesia::apps
