// SPDX-License-Identifier: Apache-2.0
// Host-only test: c++ -std=c++17 -I. test_boot_click_gesture.cpp -o /tmp/boot-click-test && /tmp/boot-click-test
#include "boot_click_gesture.hpp"

#include <cassert>
#include <vector>

using esp_brookesia::apps::BootClickGesture;

struct Fixture {
    BootClickGesture gesture;
    uint64_t now = 0;
    std::vector<BootClickGesture::Decision> decisions;

    Fixture() { gesture.update(false, now); }

    void feed(bool pressed, uint32_t duration_ms)
    {
        for (uint32_t elapsed = 0; elapsed < duration_ms; elapsed += 10) {
            now += 10;
            const auto decision = gesture.update(pressed, now);
            if (decision != BootClickGesture::Decision::None) decisions.push_back(decision);
        }
    }

    void click()
    {
        feed(true, 80);
        feed(false, 80);
    }
};

int main()
{
    using Decision = BootClickGesture::Decision;
    {
        Fixture f;
        f.click();
        f.feed(false, 300);
        assert(f.decisions.empty());
        f.feed(false, 150);
        assert((f.decisions == std::vector<Decision>{Decision::Voice}));
    }
    {
        Fixture f;
        f.click();
        f.feed(false, 330);
        f.feed(true, 130); // A second press spans the single-click deadline.
        assert(f.decisions.empty());
        f.feed(false, 550);
        assert((f.decisions == std::vector<Decision>{Decision::Confirm}));
    }
    {
        BootClickGesture gesture;
        uint64_t now = 0;
        gesture.update(true, now); // App opens while BOOT is already held.
        for (int i = 0; i < 20; ++i) gesture.update(true, now += 10);
        for (int i = 0; i < 50; ++i) gesture.update(false, now += 10);
        assert(!gesture.sequenceActive());
        for (int click = 0; click < 2; ++click) {
            for (int i = 0; i < 8; ++i) gesture.update(true, now += 10);
            for (int i = 0; i < 8; ++i) gesture.update(false, now += 10);
        }
        Decision decision = Decision::None;
        for (int i = 0; i < 50; ++i) {
            auto step = gesture.update(false, now += 10);
            if (step != Decision::None) decision = step;
        }
        assert(decision == Decision::Confirm);
    }
    {
        Fixture f;
        f.click();
        f.click();
        f.feed(false, 300);
        assert(f.decisions.empty()); // Must wait for possible third click.
        f.feed(false, 150);
        assert((f.decisions == std::vector<Decision>{Decision::Confirm}));
    }
    {
        Fixture f;
        f.click();
        f.click();
        f.click();
        f.feed(false, 500);
        assert((f.decisions == std::vector<Decision>{Decision::Cancel}));
    }
    {
        Fixture f;
        f.click();
        f.click();
        f.feed(false, 330);
        f.feed(true, 130); // The third press spans the old deadline.
        assert(f.decisions.empty());
        f.feed(false, 550);
        assert((f.decisions == std::vector<Decision>{Decision::Cancel}));
    }
    {
        Fixture f;
        f.feed(true, 10); // Contact bounce is shorter than debounce.
        f.feed(false, 40);
        f.click();
        f.click();
        f.feed(false, 500);
        assert((f.decisions == std::vector<Decision>{Decision::Confirm}));
    }
    {
        Fixture f;
        f.click();
        f.gesture.reset(); // Leaving the app clears a pending single-click too.
        f.feed(false, 600);
        assert(f.decisions.empty());
    }
    {
        Fixture f;
        f.click();
        f.click();
        f.gesture.reset(); // Leaving the app clears a pending double-click.
        f.feed(false, 600);
        assert(f.decisions.empty());
    }
    {
        Fixture f;
        f.click();
        f.click();
        f.click();
        f.click();
        f.feed(false, 600);
        assert(f.decisions.empty());
    }
    {
        Fixture f;
        f.feed(true, 1300);
        // hold 武装时长按完全归录音：500ms Hold，无 Home（实测旧版会在
        // 1.2s 发 Home 取消录音，说话必被杀）。
        assert((f.decisions == std::vector<Decision>{Decision::Hold}));
        f.feed(false, 60);
        f.feed(false, 600);
        assert((f.decisions == std::vector<Decision>{Decision::Hold, Decision::Release}));
    }
    {
        BootClickGesture gesture;
        uint64_t now = 0;
        gesture.update(true, now); // BOOT was held as the app opened.
        Decision decision = Decision::None;
        for (int i = 0; i < 130; ++i) {
            const auto step = gesture.update(true, now += 10);
            if (step != Decision::None) decision = step;
        }
        // hold 武装：开场即按住也走录音（Hold）。
        assert(decision == Decision::Hold);
        // 松手收尾 Release（旧版这里被 Home 吞掉，导致松手不发送——实测踩坑）。
        Decision release = Decision::None;
        for (int i = 0; i < 40; ++i) {
            const auto step = gesture.update(false, now += 10);
            if (step != Decision::None) release = step;
        }
        assert(release == Decision::Release);
    }
    {
        // hold 不武装（voice 不可用）：长按保留旧 Home 语义。
        Fixture f;
        f.gesture.setHoldArmed(false);
        f.feed(true, 1300);
        f.feed(false, 600);
        assert((f.decisions == std::vector<Decision>{Decision::Home}));
    }
    {
        Fixture f;
        f.click();
        f.feed(false, 120);
        f.feed(true, 1250);
        f.feed(false, 600);
        assert((f.decisions == std::vector<Decision>{Decision::Hold, Decision::Release}));
    }
    {
        Fixture f;
        f.feed(true, 900); // A medium hold now records: Hold at 500ms.
        f.feed(false, 600);
        assert((f.decisions == std::vector<Decision>{Decision::Hold, Decision::Release}));
    }
    {
        // 长按说话主流程：按住 2s 说话、松手 → Hold + Release，无 Home。
        Fixture f;
        f.feed(true, 2000);
        f.feed(false, 100);
        assert((f.decisions == std::vector<Decision>{Decision::Hold, Decision::Release}));
    }
    {
        // 按住 700ms（未到 home 阈值）松手 → 干净的 Hold + Release。
        Fixture f;
        f.feed(true, 700);
        f.feed(false, 100);
        assert((f.decisions == std::vector<Decision>{Decision::Hold, Decision::Release}));
    }
}
