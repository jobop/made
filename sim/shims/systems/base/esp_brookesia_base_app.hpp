// sim/shims/systems/base/esp_brookesia_base_app.hpp — host 替身。
// 真机上是 brookesia_core 的 base::App（含屏幕生命周期管理）；模拟器里
// VibeCoding 只用到 Config 与 run/pause/resume/back/close 虚函数，
// 默认屏幕直接用 LVGL 的 lv_scr_act()，视觉行为与真机一致。
#pragma once

#include <cstdint>
#include "lvgl.h"

namespace esp_brookesia::systems::base {

class App {
public:
    struct Config {
        const char *name = nullptr;
        const void *launcher_icon = nullptr;
        bool use_default_screen = true;
        struct {
            uint8_t enable_navigation_gesture : 1;
            uint8_t reserved : 7;
        } flags = {};

        static constexpr Config SIMPLE_CONSTRUCTOR(const char *name, const void *launcher_icon,
                                                   bool use_default_screen)
        {
            Config config;
            config.name = name;
            config.launcher_icon = launcher_icon;
            config.use_default_screen = use_default_screen;
            config.flags.enable_navigation_gesture = 0;
            return config;
        }
    };

    explicit App(const Config &config) : config_(config) {}
    virtual ~App() = default;

    virtual bool run() { return true; }
    virtual bool pause() { return true; }
    virtual bool resume() { return true; }
    virtual bool back() { return true; }
    virtual bool close() { return true; }

protected:
    Config config_;
};

} // namespace esp_brookesia::systems::base
