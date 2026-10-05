#pragma once

#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace esp_brookesia::apps {

struct WeatherSnapshot {
    std::string city;
    std::string condition;
    std::string observed_at;
    std::string error;
    float temperature_c = 0;
    bool available = false;
    bool stale = false;
};

class WeatherData final {
public:
    static WeatherData& instance();
    void start();
    bool setCity(const std::string& city);
    void refresh();
    WeatherSnapshot snapshot();

private:
    WeatherData();
    WeatherData(const WeatherData&) = delete;
    WeatherData& operator=(const WeatherData&) = delete;

    static void taskEntry(void* context);
    static bool getJson(const std::string& url, std::string& body);
    static bool geocode(const std::string& city, double& lat, double& lon);
    static bool current(double lat, double lon, WeatherSnapshot& value, int& code);
    static const char* conditionFor(int code);
    void run();
    void recordError(uint32_t revision, const char* error);
    void wait(uint32_t milliseconds);

    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    WeatherSnapshot state_;
    double latitude_ = 0;
    double longitude_ = 0;
    bool have_coordinates_ = false;
    uint32_t revision_ = 0;
};

} // namespace esp_brookesia::apps
