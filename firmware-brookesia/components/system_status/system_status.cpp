/*
 * SPDX-FileCopyrightText: 2026 Vibe Coding Bridge contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_status.hpp"

#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "i2c_bus.h"
#include "lvgl.h"
#include "vibe_wifi.hpp"

namespace brookesia::system_status {
namespace {

constexpr char kTag[] = "system_status";
constexpr uint8_t kBq27220Address = 0x55;
constexpr uint8_t kVoltageRegister = 0x08;
constexpr uint8_t kBatteryStatusRegister = 0x0A;
constexpr uint8_t kCurrentRegister = 0x0C;
constexpr uint8_t kStateOfChargeRegister = 0x2C;
constexpr uint16_t kBatteryPresentBit = 1U << 3;

using StatusBar = esp_brookesia::systems::phone::StatusBar;

portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
Snapshot latest;
bool have_snapshot = false;
StatusBar *bar = nullptr;
lv_timer_t *refresh_timer = nullptr;
i2c_bus_device_handle_t gauge = nullptr;

bool read16(uint8_t reg, uint16_t &value)
{
    uint8_t bytes[2] = {};
    if (gauge == nullptr || i2c_bus_read_bytes(gauge, reg, sizeof(bytes), bytes) != ESP_OK) {
        return false;
    }
    value = static_cast<uint16_t>(bytes[0]) |
            (static_cast<uint16_t>(bytes[1]) << 8);
    return true;
}

void refresh(lv_timer_t *)
{
    Snapshot next;
    next.wifi_connected = vibe_wifi::is_connected();
    if (next.wifi_connected) {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            next.wifi_rssi = ap.rssi;
        }
    }

    if (gauge == nullptr) {
        i2c_bus_handle_t bus = bsp_i2c_bus_get_handle();
        if (bus != nullptr) {
            gauge = i2c_bus_device_create(bus, kBq27220Address, 0);
        }
    }

    uint16_t status = 0;
    uint16_t percent = 0;
    uint16_t voltage = 0;
    uint16_t current = 0;
    if (read16(kBatteryStatusRegister, status) &&
        read16(kStateOfChargeRegister, percent) &&
        read16(kVoltageRegister, voltage) &&
        read16(kCurrentRegister, current) && percent <= 100) {
        next.battery_valid = true;
        next.battery_present = (status & kBatteryPresentBit) != 0;
        next.battery_percent = percent;
        next.battery_mv = voltage;
        next.battery_ma = static_cast<int16_t>(current);
        next.charging = next.battery_ma > 0;
    }

    portENTER_CRITICAL(&snapshot_lock);
    latest = next;
    have_snapshot = true;
    portEXIT_CRITICAL(&snapshot_lock);

    if (bar == nullptr) return;
    if (next.battery_valid && next.battery_present) {
        bar->setBatteryPercent(next.charging, next.battery_percent);
    }
    using WifiState = StatusBar::WifiState;
    WifiState wifi_state = WifiState::DISCONNECTED;
    if (next.wifi_connected) {
        wifi_state = next.wifi_rssi >= -60 ? WifiState::SIGNAL_3 :
                     next.wifi_rssi >= -75 ? WifiState::SIGNAL_2 : WifiState::SIGNAL_1;
    }
    bar->setWifiIconState(wifi_state);
}

} // namespace

esp_err_t start(StatusBar *status_bar, uint32_t refresh_period_ms)
{
    if (status_bar == nullptr || refresh_period_ms < 1000) return ESP_ERR_INVALID_ARG;
    if (refresh_timer != nullptr) return ESP_ERR_INVALID_STATE;
    bar = status_bar;
    refresh_timer = lv_timer_create(refresh, refresh_period_ms, nullptr);
    if (refresh_timer == nullptr) {
        bar = nullptr;
        return ESP_ERR_NO_MEM;
    }
    refresh(nullptr);
    ESP_LOGI(kTag, "1.85B status monitor started");
    return ESP_OK;
}

void stop()
{
    if (refresh_timer != nullptr) {
        lv_timer_delete(refresh_timer);
        refresh_timer = nullptr;
    }
    bar = nullptr;
    if (gauge != nullptr) {
        i2c_bus_device_delete(&gauge);
    }
}

bool get_snapshot(Snapshot &snapshot)
{
    portENTER_CRITICAL(&snapshot_lock);
    const bool available = have_snapshot;
    if (available) snapshot = latest;
    portEXIT_CRITICAL(&snapshot_lock);
    return available;
}

} // namespace brookesia::system_status
