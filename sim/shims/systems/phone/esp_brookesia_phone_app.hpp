// sim/shims/systems/phone/esp_brookesia_phone_app.hpp — host 替身。
// 真机的 phone::App 带状态栏/导航栏/最近任务框架；Muse Charm 形态的
// VibeCoding 不使用它们（status_bar/navigation_bar 全关），因此替身只保留
// 构造签名与 flags，UI 全部由应用自己画在 lv_scr_act() 上。
#pragma once

#include "systems/base/esp_brookesia_base_app.hpp"

namespace esp_brookesia::systems::phone {

class App : public base::App {
public:
    struct Config {
        struct {
            uint8_t enable_status_icon_common_size : 1;
            uint8_t enable_navigation_gesture : 1;
            uint8_t reserved : 6;
        } flags = {};

        static constexpr Config SIMPLE_CONSTRUCTOR(const void *status_icon, bool use_status_bar,
                                                   bool use_navigation_bar)
        {
            (void)status_icon;
            (void)use_status_bar;
            (void)use_navigation_bar;
            Config config;
            config.flags.enable_status_icon_common_size = 0;
            config.flags.enable_navigation_gesture = 1;
            return config;
        }
    };

    App(const base::App::Config &core_data, const Config &phone_data)
        : base::App(core_data), phone_config_(phone_data) {}

    const Config &getInitConfig() const { return phone_config_; }
    const Config &getActiveConfig() const { return phone_config_; }

private:
    Config phone_config_;
};

} // namespace esp_brookesia::systems::phone
