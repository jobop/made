// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace vibe_voice {

// ES7210 exposes the two physical microphones in TDM slots 0 and 2. XiaoZhi
// consumes both; Vibe needs mono PCM. Choose the stronger microphone per block
// so a silent/disconnected channel cannot hide a valid spoken command.
struct MicLevels {
    uint32_t mic1 = 0;
    uint32_t mic2 = 0;
    unsigned selected_slot = 0;
};

inline uint32_t micLevel(const int16_t *tdm, size_t frames, size_t slot)
{
    if (frames == 0) return 0;
    int64_t sum = 0;
    for (size_t frame = 0; frame < frames; ++frame) sum += tdm[frame * 4 + slot];
    const int32_t mean = static_cast<int32_t>(sum / static_cast<int64_t>(frames));
    uint64_t deviation = 0;
    for (size_t frame = 0; frame < frames; ++frame) {
        const int32_t centered = static_cast<int32_t>(tdm[frame * 4 + slot]) - mean;
        deviation += centered < 0 ? -centered : centered;
    }
    return static_cast<uint32_t>(deviation / frames);
}

inline MicLevels selectMic(const int16_t *tdm, size_t frames, int16_t *mono)
{
    MicLevels levels{micLevel(tdm, frames, 0), micLevel(tdm, frames, 2), 0};
    // Prefer slot 0 when both channels carry comparable audio, preserving
    // continuity with older recordings. Switch to slot 2 when clearly louder.
    if (levels.mic2 > levels.mic1 + std::max<uint32_t>(40, levels.mic1 / 2)) {
        levels.selected_slot = 2;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        mono[frame] = tdm[frame * 4 + levels.selected_slot];
    }
    return levels;
}

} // namespace vibe_voice
