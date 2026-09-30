// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace vibe_voice {

// The board's 24 dB microphone setting still produces unusually small PCM
// amplitudes. Increase only the finished utterance sent to the PC, leaving
// the original samples used by the endpoint detector untouched.
inline uint32_t amplifyForUpload(int16_t* pcm, size_t frames)
{
    if (pcm == nullptr || frames == 0) return 1;

    uint64_t sum_absolute = 0;
    for (size_t i = 0; i < frames; ++i) {
        const int32_t sample = pcm[i];
        sum_absolute += sample < 0 ? -sample : sample;
    }
    const uint32_t mean_absolute = static_cast<uint32_t>(sum_absolute / frames);
    const uint32_t gain = std::min<uint32_t>(12, std::max<uint32_t>(1,
        600 / std::max<uint32_t>(1, mean_absolute)));

    for (size_t i = 0; i < frames; ++i) {
        const int32_t scaled = static_cast<int32_t>(pcm[i]) * static_cast<int32_t>(gain);
        pcm[i] = static_cast<int16_t>(std::clamp<int32_t>(scaled, -32768, 32767));
    }
    return gain;
}

} // namespace vibe_voice
