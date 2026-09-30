/*
 * SPDX-FileCopyrightText: 2026 Vibe Coding Bridge contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

#include "esp_err.h"

namespace esp_brookesia::systems::phone {
class StatusBar;
}

namespace brookesia::system_status {

struct Snapshot {
    bool battery_valid = false;
    bool battery_present = false;
    int battery_percent = -1;
    int battery_mv = -1;
    int battery_ma = 0;
    bool charging = false;
    bool wifi_connected = false;
    int wifi_rssi = -127;
};

// Call while holding the Brookesia/LVGL display lock, after Phone::begin().
// The status bar must outlive this monitor. The BQ27220 is read without
// touching its calibration profile or the Wi-Fi connection policy.
esp_err_t start(esp_brookesia::systems::phone::StatusBar *status_bar,
                uint32_t refresh_period_ms = 5000);
void stop();
bool get_snapshot(Snapshot &snapshot);

} // namespace brookesia::system_status
