// SPDX-License-Identifier: Apache-2.0
// Host-only: c++ -std=c++17 -Iinclude test_pcm_upload_gain.cpp -o /tmp/pcm-gain-test && /tmp/pcm-gain-test
#include "pcm_upload_gain.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

int main()
{
    {
        std::array<int16_t, 2> pcm{50, -50};
        assert(vibe_voice::amplifyForUpload(pcm.data(), pcm.size()) == 12);
        assert(pcm[0] == 600 && pcm[1] == -600);
    }
    {
        std::array<int16_t, 2> pcm{100, -100};
        assert(vibe_voice::amplifyForUpload(pcm.data(), pcm.size()) == 6);
        assert(pcm[0] == 600 && pcm[1] == -600);
    }
    {
        std::array<int16_t, 2> pcm{1000, -1000};
        assert(vibe_voice::amplifyForUpload(pcm.data(), pcm.size()) == 1);
        assert(pcm[0] == 1000 && pcm[1] == -1000);
    }
    {
        std::array<int16_t, 1000> pcm{};
        for (size_t i = 0; i < pcm.size() - 2; ++i) pcm[i] = i & 1 ? 50 : -50;
        pcm[998] = 10000;
        pcm[999] = -10000;
        assert(vibe_voice::amplifyForUpload(pcm.data(), pcm.size()) == 8);
        assert(pcm[0] == -400 && pcm[1] == 400);
        assert(pcm[998] == std::numeric_limits<int16_t>::max());
        assert(pcm[999] == std::numeric_limits<int16_t>::min());
    }
    {
        std::array<int16_t, 2> pcm{0, std::numeric_limits<int16_t>::min()};
        assert(vibe_voice::amplifyForUpload(pcm.data(), pcm.size()) == 1);
        assert(pcm[0] == 0 && pcm[1] == std::numeric_limits<int16_t>::min());
    }
    assert(vibe_voice::amplifyForUpload(nullptr, 0) == 1);
}
