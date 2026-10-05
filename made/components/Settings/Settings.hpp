/*
 * Adapted for ESP32-S3-Touch-LCD-1.85B from Waveshare's ESP32-S3-Touch-AMOLED-1.75 Settings app.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "esp_wifi.h"
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class Settings final : public systems::phone::App {
public:
    static Settings *requestInstance();
    ~Settings() override = default;

protected:
    Settings();
    bool init() override;
    bool deinit() override;
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    enum class Page { Main, Wifi, Password, Display, Sound, Power, About };
    struct ScanContext { Settings *self; uint32_t generation; };

    static Settings *instance_;
    static void backButton(lv_event_t *event);
    static void menuButton(lv_event_t *event);
    static void scanButton(lv_event_t *event);
    static void networkButton(lv_event_t *event);
    static void connectButton(lv_event_t *event);
    static void keyboardReady(lv_event_t *event);
    static void brightnessChanged(lv_event_t *event);
    static void powerTimer(lv_timer_t *timer);
    static void scanTask(void *arg);

    void clearPage();
    void createPage(const char *title, bool with_list = true);
    void showMain();
    void showWifi();
    void showPassword();
    void showDisplay();
    void showSound();
    void showPower();
    void showAbout();
    void refreshWifiList();
    void refreshPower();
    void startScan();
    void connectSelected();
    lv_obj_t *addMenuItem(const char *name, Page page);

    Page page_ = Page::Main;
    lv_obj_t *root_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *wifi_status_ = nullptr;
    lv_obj_t *password_input_ = nullptr;
    lv_obj_t *password_status_ = nullptr;
    lv_obj_t *brightness_label_ = nullptr;
    lv_obj_t *power_battery_ = nullptr;
    lv_obj_t *power_voltage_ = nullptr;
    lv_obj_t *power_current_ = nullptr;
    lv_obj_t *power_network_ = nullptr;
    lv_timer_t *page_timer_ = nullptr;
    lv_style_t style_list_{};
    lv_style_t style_row_{};
    lv_style_t style_section_{};
    lv_style_t style_pressed_{};
    bool styles_ready_ = false;
    std::atomic<bool> scanning_{false};
    uint32_t page_generation_ = 0;
    wifi_ap_record_t access_points_[8]{};
    uint16_t access_point_count_ = 0;
    uint32_t wifi_static_rows_ = 0;
    char selected_ssid_[33]{};
    int brightness_ = 100;
};

} // namespace esp_brookesia::apps
