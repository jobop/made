#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class Weather final : public systems::phone::App {
public:
    static Weather* requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

protected:
    Weather(bool use_status_bar, bool use_navigation_bar);
    bool init() override;
    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void tick(lv_timer_t* timer);
    static void openEditor(lv_event_t* event);
    static void saveCity(lv_event_t* event);
    static void dismissEditor(lv_event_t* event);
    static void refreshWeather(lv_event_t* event);
    void updateView();
    void showEditor(bool show);

    static Weather* instance_;
    lv_obj_t* city_label_ = nullptr;
    lv_obj_t* temperature_label_ = nullptr;
    lv_obj_t* condition_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* editor_ = nullptr;
    lv_obj_t* city_input_ = nullptr;
    lv_timer_t* timer_ = nullptr;
};

} // namespace esp_brookesia::apps
