// SPDX-License-Identifier: Apache-2.0
// Host-only: c++ -std=c++17 -Iinclude test_tdm_mic_selector.cpp -o /tmp/tdm-mic-test && /tmp/tdm-mic-test
#include "tdm_mic_selector.hpp"

#include <array>
#include <cassert>

int main()
{
    std::array<int16_t, 256 * 4> tdm{};
    std::array<int16_t, 256> mono{};
    for (size_t frame = 0; frame < mono.size(); ++frame) {
        tdm[frame * 4 + 0] = frame & 1 ? 8 : -8;
        tdm[frame * 4 + 1] = frame & 1 ? 20000 : -20000; // Echo reference is never selected.
        tdm[frame * 4 + 2] = frame & 1 ? 800 : -800;
    }
    auto levels = vibe_voice::selectMic(tdm.data(), mono.size(), mono.data());
    assert(levels.selected_slot == 2);
    assert(levels.mic1 == 8 && levels.mic2 == 800);
    assert(mono[0] == -800 && mono[1] == 800);

    for (size_t frame = 0; frame < mono.size(); ++frame) {
        tdm[frame * 4 + 0] = frame & 1 ? 1100 : -1100;
        tdm[frame * 4 + 2] = frame & 1 ? 900 : -900;
    }
    levels = vibe_voice::selectMic(tdm.data(), mono.size(), mono.data());
    assert(levels.selected_slot == 0);
    assert(mono[0] == -1100 && mono[1] == 1100);
}
