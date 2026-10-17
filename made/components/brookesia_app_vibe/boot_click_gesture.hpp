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
    enum class Decision { None, Voice, Confirm, Cancel, Hold, Release };

    static constexpr uint32_t debounce_ms = 30;
    static constexpr uint32_t click_gap_ms = 420;
    static constexpr uint32_t max_press_ms = 650;
    // 长按说话：按住该时长即进入录音（Hold），松手结束并发送（Release）。
    // 长按完全归属录音，无 Home 分支（2026-10-17 用户要求移除：说话期间
    // 任何长按衍生决策都会干扰录音）；取消录音用三击。
    static constexpr uint32_t voice_hold_ms = 500;

    void reset()
    {
        initialized_ = false;
        raw_pressed_ = false;
        stable_pressed_ = false;
        ignore_initial_press_ = false;
        hold_sent_ = false;
        hold_armed_ = true;
        clicks_ = 0;
        raw_changed_ms_ = 0;
        press_started_ms_ = 0;
        last_released_ms_ = 0;
    }

    // App 层每轮喂：voice 不可用/设置打开时 hold 不武装，长按无动作。
    void setHoldArmed(bool armed) { hold_armed_ = armed; }

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
            } else if (hold_sent_) {
                // 长按录音的松手：结束录音并上传（app 层调 vibe_voice::finish()）。
                hold_sent_ = false;
                ignore_initial_press_ = false;
                clicks_ = 0;
                return Decision::Release;
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

        // 长按说话：按住达到阈值即开始录音，松手由 Release 分支收尾。
        // 快速单击/双击/三击因按压时长不足而不触发。
        if (hold_armed_ && stable_pressed_ && !hold_sent_ &&
            now_ms - press_started_ms_ >= voice_hold_ms) {
            hold_sent_ = true;
            clicks_ = 0;
            return Decision::Hold;
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
    bool hold_sent_ = false;
    bool hold_armed_ = true;
    unsigned clicks_ = 0;
    uint64_t raw_changed_ms_ = 0;
    uint64_t press_started_ms_ = 0;
    uint64_t last_released_ms_ = 0;
};

} // namespace esp_brookesia::apps
