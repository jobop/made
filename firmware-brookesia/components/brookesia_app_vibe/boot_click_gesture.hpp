// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace esp_brookesia::apps {

// Feed the raw active-low button as a boolean at least every 20 ms.
// A click sequence is committed only after a full next-click window has
// elapsed with the button released. A press already in progress keeps the
// window open, so a slow follow-up release cannot trigger an earlier action.
class BootClickGesture {
public:
    enum class Decision { None, Voice, Confirm, Cancel, Home };

    static constexpr uint32_t debounce_ms = 30;
    static constexpr uint32_t click_gap_ms = 420;
    static constexpr uint32_t max_press_ms = 650;
    static constexpr uint32_t home_press_ms = 1200;

    void reset()
    {
        initialized_ = false;
        raw_pressed_ = false;
        stable_pressed_ = false;
        ignore_initial_press_ = false;
        home_sent_ = false;
        clicks_ = 0;
        raw_changed_ms_ = 0;
        press_started_ms_ = 0;
        last_released_ms_ = 0;
    }

    bool sequenceActive() const { return stable_pressed_ || clicks_ > 0; }

    Decision update(bool pressed, uint64_t now_ms)
    {
        if (!initialized_) {
            initialized_ = true;
            raw_pressed_ = pressed;
            stable_pressed_ = pressed;
            ignore_initial_press_ = pressed;
            raw_changed_ms_ = now_ms;
            press_started_ms_ = now_ms;
            return Decision::None;
        }

        if (pressed != raw_pressed_) {
            raw_pressed_ = pressed;
            raw_changed_ms_ = now_ms;
        }
        if (raw_pressed_ != stable_pressed_ &&
            now_ms - raw_changed_ms_ >= debounce_ms) {
            stable_pressed_ = raw_pressed_;
            if (stable_pressed_) {
                press_started_ms_ = now_ms;
                home_sent_ = false;
            } else if (home_sent_) {
                // The hold already requested exit. Its release is not a click.
                home_sent_ = false;
                ignore_initial_press_ = false;
                clicks_ = 0;
            } else if (now_ms - press_started_ms_ >= home_press_ms) {
                // Fallback if the task sampled the release just after the
                // threshold and missed the exact held-state transition.
                ignore_initial_press_ = false;
                clicks_ = 0;
                return Decision::Home;
            } else if (ignore_initial_press_) {
                ignore_initial_press_ = false;
                clicks_ = 0;
            } else if (now_ms - press_started_ms_ <= max_press_ms) {
                if (clicks_ > 0 && press_started_ms_ - last_released_ms_ > click_gap_ms) {
                    clicks_ = 0;
                }
                ++clicks_;
                last_released_ms_ = now_ms;
            } else {
                clicks_ = 0;
            }
        }

        // Trigger while BOOT is held. Requiring a release made a long press
        // appear broken on the physical board. A press already held when the
        // listener starts can still exit after the full hold interval.
        if (stable_pressed_ && !home_sent_ &&
            now_ms - press_started_ms_ >= home_press_ms) {
            home_sent_ = true;
            clicks_ = 0;
            return Decision::Home;
        }

        // raw_pressed_ guards against confirming during a third press that
        // has begun but has not yet passed the debounce interval.
        if (!raw_pressed_ && !stable_pressed_ && clicks_ > 0 &&
            now_ms - last_released_ms_ >= click_gap_ms) {
            const unsigned completed = clicks_;
            clicks_ = 0;
            if (completed == 1) return Decision::Voice;
            if (completed == 2) return Decision::Confirm;
            if (completed == 3) return Decision::Cancel;
        }
        return Decision::None;
    }

private:
    bool initialized_ = false;
    bool raw_pressed_ = false;
    bool stable_pressed_ = false;
    bool ignore_initial_press_ = false;
    bool home_sent_ = false;
    unsigned clicks_ = 0;
    uint64_t raw_changed_ms_ = 0;
    uint64_t press_started_ms_ = 0;
    uint64_t last_released_ms_ = 0;
};

} // namespace esp_brookesia::apps
