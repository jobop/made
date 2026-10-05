/*
 * Adapted for ESP32-S3-Touch-LCD-1.85B from Waveshare's
 * ESP32-S3-Touch-AMOLED-1.75 Settings app.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "Settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system_status.hpp"
#include "ui/SettingsUI.hpp"
#include "vibe_wifi.hpp"
#include "vibe_touch_keyboard.hpp"

LV_IMG_DECLARE(img_app_settings);

namespace esp_brookesia::apps {
namespace {
constexpr char kTag[] = "BS:Settings";
constexpr uint16_t kMaxNetworks = 8;

lv_obj_t *rowValue(lv_obj_t *row)
{
    const uint32_t count = lv_obj_get_child_count(row);
    return count > 0 ? lv_obj_get_child(row, static_cast<int32_t>(count - 1)) : nullptr;
}
} // namespace

Settings *Settings::instance_ = nullptr;

Settings *Settings::requestInstance()
{
    if (instance_ == nullptr) instance_ = new Settings();
    return instance_;
}

Settings::Settings()
    : App("Settings", &img_app_settings, true, true, false)
{
}

bool Settings::init()
{
    // The resident Wi-Fi service is shared with Weather and Vibe Coding.
    // It may already be running before this app is opened.
    return vibe_wifi::start() == ESP_OK;
}

bool Settings::deinit() { return true; }
bool Settings::pause() { return true; }
bool Settings::resume() { return true; }

void Settings::clearPage()
{
    ++page_generation_;
    if (page_timer_ != nullptr) {
        lv_timer_delete(page_timer_);
        page_timer_ = nullptr;
    }
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    if (styles_ready_) {
        settings_ui::reset_list_styles(style_list_, style_row_, style_section_, style_pressed_);
        styles_ready_ = false;
    }
    list_ = nullptr;
    wifi_status_ = nullptr;
    password_input_ = nullptr;
    password_status_ = nullptr;
    brightness_label_ = nullptr;
    power_battery_ = nullptr;
    power_voltage_ = nullptr;
    power_current_ = nullptr;
    power_network_ = nullptr;
    wifi_static_rows_ = 0;
}

void Settings::createPage(const char *title, bool with_list)
{
    clearPage();
    lv_obj_clean(lv_screen_active());
    root_ = settings_ui::create_page(lv_screen_active());
    settings_ui::create_header(root_, title, backButton, this);
    settings_ui::init_list_styles(style_list_, style_row_, style_section_, style_pressed_);
    styles_ready_ = true;
    if (with_list) {
        list_ = settings_ui::create_content_list(root_);
        lv_obj_add_style(list_, &style_list_, LV_PART_MAIN);
    }
}

lv_obj_t *Settings::addMenuItem(const char *name, Page page)
{
    lv_obj_t *button = lv_list_add_button(list_, nullptr, name);
    lv_obj_add_style(button, &style_row_, LV_PART_MAIN);
    lv_obj_add_style(button, &style_pressed_, LV_STATE_PRESSED);
    settings_ui::use_ellipsis_for_button_label(button);
    lv_obj_add_event_cb(button, menuButton, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(page)));
    return button;
}

void Settings::showMain()
{
    page_ = Page::Main;
    createPage("Settings");
    settings_ui::add_section(list_, "Wireless", style_section_);
    addMenuItem("WLAN", Page::Wifi);
    settings_ui::add_section(list_, "Device", style_section_);
    addMenuItem("Display", Page::Display);
    addMenuItem("Sound", Page::Sound);
    addMenuItem("Power", Page::Power);
    settings_ui::add_section(list_, "More", style_section_);
    addMenuItem("About", Page::About);
}

void Settings::showWifi()
{
    page_ = Page::Wifi;
    createPage("WLAN");
    settings_ui::add_section(list_, "Connection", style_section_);
    wifi_status_ = lv_list_add_text(list_, vibe_wifi::is_connected() ? "Connected" : "Not connected");
    lv_obj_add_style(wifi_status_, &style_section_, LV_PART_MAIN);
    if (vibe_wifi::is_connected()) {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            char text[72];
            std::snprintf(text, sizeof(text), "Connected: %.32s",
                          reinterpret_cast<const char *>(ap.ssid));
            lv_label_set_text(wifi_status_, text);
        }
    }
    lv_obj_t *scan = lv_list_add_button(list_, LV_SYMBOL_REFRESH, "Scan networks");
    lv_obj_add_style(scan, &style_row_, LV_PART_MAIN);
    lv_obj_add_style(scan, &style_pressed_, LV_STATE_PRESSED);
    settings_ui::use_ellipsis_for_button_label(scan);
    lv_obj_add_event_cb(scan, scanButton, LV_EVENT_CLICKED, this);
    settings_ui::add_section(list_, "Available", style_section_);
    wifi_static_rows_ = lv_obj_get_child_count(list_);
    refreshWifiList();
    startScan();
}

void Settings::refreshWifiList()
{
    if (page_ != Page::Wifi || list_ == nullptr) return;
    while (lv_obj_get_child_count(list_) > wifi_static_rows_) {
        lv_obj_delete(lv_obj_get_child(list_, static_cast<int32_t>(wifi_static_rows_)));
    }
    if (access_point_count_ == 0) {
        lv_obj_t *hint = lv_list_add_text(list_, scanning_.load() ? "Scanning..." : "No networks found");
        lv_obj_add_style(hint, &style_section_, LV_PART_MAIN);
        return;
    }
    for (uint16_t i = 0; i < access_point_count_; ++i) {
        const wifi_ap_record_t &ap = access_points_[i];
        char label[64];
        std::snprintf(label, sizeof(label), "%.32s  %s",
                      reinterpret_cast<const char *>(ap.ssid),
                      ap.authmode == WIFI_AUTH_OPEN ? "Open" : "Locked");
        lv_obj_t *button = lv_list_add_button(list_, nullptr, label);
        lv_obj_add_style(button, &style_row_, LV_PART_MAIN);
        lv_obj_add_style(button, &style_pressed_, LV_STATE_PRESSED);
        settings_ui::use_ellipsis_for_button_label(button);
        lv_obj_add_event_cb(button, networkButton, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1)));
    }
}

void Settings::startScan()
{
    if (scanning_.exchange(true)) return;
    if (wifi_status_ != nullptr && !vibe_wifi::is_connected()) {
        lv_label_set_text(wifi_status_, "Scanning...");
    }
    refreshWifiList();
    auto *context = new (std::nothrow) ScanContext{this, page_generation_};
    if (context == nullptr ||
        xTaskCreate(scanTask, "settings_wifi_scan", 6144, context, 4, nullptr) != pdPASS) {
        delete context;
        scanning_.store(false);
        if (wifi_status_ != nullptr) lv_label_set_text(wifi_status_, "Scan unavailable");
    }
}

void Settings::scanTask(void *arg)
{
    auto *context = static_cast<ScanContext *>(arg);
    wifi_scan_config_t config = {};
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    const esp_err_t scan_result = esp_wifi_scan_start(&config, true);
    wifi_ap_record_t records[kMaxNetworks] = {};
    uint16_t count = kMaxNetworks;
    esp_err_t records_result = scan_result;
    if (scan_result == ESP_OK) {
        records_result = esp_wifi_scan_get_ap_records(&count, records);
    }
    if (bsp_display_lock(1000) == ESP_OK) {
        Settings *self = context->self;
        self->scanning_.store(false);
        // A scan may finish after the user briefly visits another page and
        // returns to WLAN. Its AP list is still valid for that new page.
        if (self->page_ == Page::Wifi) {
            if (scan_result == ESP_OK && records_result == ESP_OK) {
                self->access_point_count_ = 0;
                for (uint16_t i = 0; i < count && self->access_point_count_ < kMaxNetworks; ++i) {
                    if (records[i].ssid[0] != 0) {
                        self->access_points_[self->access_point_count_++] = records[i];
                    }
                }
                if (self->wifi_status_ != nullptr && !vibe_wifi::is_connected()) {
                    lv_label_set_text(self->wifi_status_, "Choose a network");
                }
            } else if (self->wifi_status_ != nullptr) {
                lv_label_set_text(self->wifi_status_, "Scan failed. Tap to retry.");
                ESP_LOGW(kTag, "Wi-Fi scan failed: %s", esp_err_to_name(scan_result));
            }
            self->refreshWifiList();
        }
        bsp_display_unlock();
    } else {
        context->self->scanning_.store(false);
    }
    delete context;
    vTaskDelete(nullptr);
}

void Settings::showPassword()
{
    page_ = Page::Password;
    createPage("Join Wi-Fi", false);
    lv_obj_t *name = lv_label_create(root_);
    lv_label_set_text(name, selected_ssid_);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, 250);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 91);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(settings_ui::COLOR_PRIMARY_TEXT), LV_PART_MAIN);

    password_input_ = lv_textarea_create(root_);
    lv_obj_set_size(password_input_, 165, 40);
    lv_obj_align(password_input_, LV_ALIGN_TOP_LEFT, 52, 115);
    lv_textarea_set_one_line(password_input_, true);
    lv_textarea_set_password_mode(password_input_, true);
    lv_textarea_set_max_length(password_input_, 63);
    lv_textarea_set_placeholder_text(password_input_, "Password");
    lv_obj_add_event_cb(password_input_, keyboardReady, LV_EVENT_READY, this);

    lv_obj_t *connect = lv_button_create(root_);
    lv_obj_set_size(connect, 78, 40);
    lv_obj_align(connect, LV_ALIGN_TOP_LEFT, 225, 115);
    lv_obj_add_event_cb(connect, connectButton, LV_EVENT_CLICKED, this);
    lv_obj_t *connect_label = lv_label_create(connect);
    lv_label_set_text(connect_label, "Join");
    lv_obj_center(connect_label);

    password_status_ = lv_label_create(root_);
    lv_label_set_text(password_status_, "Enter password, then Join");
    lv_obj_set_width(password_status_, 260);
    lv_obj_align(password_status_, LV_ALIGN_TOP_MID, 0, 312);
    lv_obj_set_style_text_align(password_status_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(password_status_, &lv_font_montserrat_12, LV_PART_MAIN);

    // Five 48 px wide keys per row replace LVGL's narrow 10-13 key rows.
    (void)vibe_touch_keyboard::create(root_, password_input_, 54, 159, 252, 148);
}

void Settings::connectSelected()
{
    if (page_ != Page::Password || password_input_ == nullptr) return;
    const char *password = lv_textarea_get_text(password_input_);
    const esp_err_t result = vibe_wifi::save_credentials(selected_ssid_, password);
    if (result != ESP_OK) {
        if (password_status_ != nullptr) {
            lv_label_set_text(password_status_, "Use 8-63 characters for secured Wi-Fi");
        }
        ESP_LOGW(kTag, "Save Wi-Fi credentials failed: %s", esp_err_to_name(result));
        return;
    }
    showWifi();
    if (wifi_status_ != nullptr) lv_label_set_text(wifi_status_, "Saved; connecting...");
}

void Settings::showDisplay()
{
    page_ = Page::Display;
    createPage("Display");
    settings_ui::add_section(list_, "Brightness", style_section_);
    lv_obj_t *panel = lv_obj_create(list_);
    lv_obj_add_style(panel, &style_row_, LV_PART_MAIN);
    lv_obj_set_size(panel, lv_pct(100), 110);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    brightness_label_ = lv_label_create(panel);
    lv_label_set_text_fmt(brightness_label_, "%d%%", brightness_);
    lv_obj_align(brightness_label_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *slider = lv_slider_create(panel);
    lv_obj_set_width(slider, lv_pct(90));
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, brightness_, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightnessChanged, LV_EVENT_VALUE_CHANGED, this);
    settings_ui::add_section(list_, "Changes apply until restart", style_section_);
}

void Settings::showSound()
{
    page_ = Page::Sound;
    createPage("Sound");
    settings_ui::add_section(list_, "Audio", style_section_);
    settings_ui::add_info_row(list_, nullptr, "Output", "ES8311", style_row_);
    settings_ui::add_section(list_, "Volume is controlled by media apps", style_section_);
}

void Settings::showPower()
{
    page_ = Page::Power;
    createPage("Power");
    settings_ui::add_section(list_, "BQ27220 battery", style_section_);
    power_battery_ = rowValue(settings_ui::add_info_row(list_, nullptr, "Level", "--", style_row_));
    power_voltage_ = rowValue(settings_ui::add_info_row(list_, nullptr, "Voltage", "--", style_row_));
    power_current_ = rowValue(settings_ui::add_info_row(list_, nullptr, "Current", "--", style_row_));
    settings_ui::add_section(list_, "Network", style_section_);
    power_network_ = rowValue(settings_ui::add_info_row(list_, nullptr, "Wi-Fi", "--", style_row_));
    refreshPower();
    page_timer_ = lv_timer_create(powerTimer, 5000, this);
}

void Settings::refreshPower()
{
    if (page_ != Page::Power) return;
    brookesia::system_status::Snapshot snapshot;
    const bool ready = brookesia::system_status::get_snapshot(snapshot);
    if (power_battery_ != nullptr) {
        if (ready && snapshot.battery_valid && snapshot.battery_present) {
            lv_label_set_text_fmt(power_battery_, "%d%%", snapshot.battery_percent);
        } else {
            lv_label_set_text(power_battery_, "Unavailable");
        }
    }
    if (power_voltage_ != nullptr) {
        if (ready && snapshot.battery_valid) {
            lv_label_set_text_fmt(power_voltage_, "%d mV", snapshot.battery_mv);
        } else {
            lv_label_set_text(power_voltage_, "--");
        }
    }
    if (power_current_ != nullptr) {
        if (ready && snapshot.battery_valid) {
            lv_label_set_text_fmt(power_current_, "%d mA", snapshot.battery_ma);
        } else {
            lv_label_set_text(power_current_, "--");
        }
    }
    if (power_network_ != nullptr) {
        lv_label_set_text(power_network_, vibe_wifi::is_connected() ? "Connected" : "Offline");
    }
}

void Settings::showAbout()
{
    page_ = Page::About;
    createPage("About");
    settings_ui::add_section(list_, "Device", style_section_);
    settings_ui::add_info_row(list_, nullptr, "Board", "1.85B", style_row_);
#if CONFIG_BSP_BOARD_LCD_2_8
    settings_ui::add_info_row(list_, nullptr, "Screen", "240 x 320", style_row_);
#else
    settings_ui::add_info_row(list_, nullptr, "Screen", "360 x 360", style_row_);
#endif
    settings_ui::add_section(list_, "Software", style_section_);
    settings_ui::add_info_row(list_, nullptr, "Desktop", "Brookesia", style_row_);
    settings_ui::add_info_row(list_, nullptr, "Bridge", "Vibe Coding", style_row_);
}

bool Settings::run()
{
    showMain();
    return true;
}

bool Settings::back()
{
    if (page_ == Page::Main) return notifyCoreClosed();
    if (page_ == Page::Password) showWifi();
    else showMain();
    return true;
}

bool Settings::close()
{
    clearPage();
    return true;
}

void Settings::backButton(lv_event_t *event)
{
    auto *self = static_cast<Settings *>(lv_event_get_user_data(event));
    if (self != nullptr) {
        lv_async_call([](void *arg) { static_cast<Settings *>(arg)->back(); }, self);
    }
}

void Settings::menuButton(lv_event_t *event)
{
    const Page page = static_cast<Page>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    lv_async_call([](void *arg) {
        auto *self = Settings::requestInstance();
        const Page next = static_cast<Page>(reinterpret_cast<uintptr_t>(arg));
        switch (next) {
        case Page::Wifi: self->showWifi(); break;
        case Page::Display: self->showDisplay(); break;
        case Page::Sound: self->showSound(); break;
        case Page::Power: self->showPower(); break;
        case Page::About: self->showAbout(); break;
        default: break;
        }
    }, reinterpret_cast<void *>(static_cast<uintptr_t>(page)));
}

void Settings::scanButton(lv_event_t *event)
{
    auto *self = static_cast<Settings *>(lv_event_get_user_data(event));
    if (self != nullptr) self->startScan();
}

void Settings::networkButton(lv_event_t *event)
{
    const uintptr_t one_based = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    lv_async_call([](void *arg) {
        Settings *self = Settings::requestInstance();
        const uintptr_t index_one_based = reinterpret_cast<uintptr_t>(arg);
        if (self->page_ != Page::Wifi || index_one_based == 0 ||
            index_one_based > self->access_point_count_) return;
        const wifi_ap_record_t &ap = self->access_points_[index_one_based - 1];
        std::memset(self->selected_ssid_, 0, sizeof(self->selected_ssid_));
        std::memcpy(self->selected_ssid_, ap.ssid,
                    std::min<size_t>(strnlen(reinterpret_cast<const char *>(ap.ssid),
                                             sizeof(ap.ssid)), 32));
        if (ap.authmode == WIFI_AUTH_OPEN) {
            const esp_err_t result = vibe_wifi::save_credentials(self->selected_ssid_, "");
            if (self->wifi_status_ != nullptr) {
                lv_label_set_text(self->wifi_status_,
                                  result == ESP_OK ? "Connecting..." : "Connection failed");
            }
        } else {
            self->showPassword();
        }
    }, reinterpret_cast<void *>(one_based));
}

void Settings::connectButton(lv_event_t *event)
{
    auto *self = static_cast<Settings *>(lv_event_get_user_data(event));
    if (self != nullptr) {
        lv_async_call([](void *arg) { static_cast<Settings *>(arg)->connectSelected(); }, self);
    }
}

void Settings::keyboardReady(lv_event_t *event)
{
    connectButton(event);
}

void Settings::brightnessChanged(lv_event_t *event)
{
    auto *self = static_cast<Settings *>(lv_event_get_user_data(event));
    if (self == nullptr || self->page_ != Page::Display) return;
    self->brightness_ = lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(event)));
    (void)bsp_display_brightness_set(self->brightness_);
    if (self->brightness_label_ != nullptr) {
        lv_label_set_text_fmt(self->brightness_label_, "%d%%", self->brightness_);
    }
}

void Settings::powerTimer(lv_timer_t *timer)
{
    auto *self = static_cast<Settings *>(lv_timer_get_user_data(timer));
    if (self != nullptr) self->refreshPower();
}

} // namespace esp_brookesia::apps
