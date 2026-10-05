// SPDX-License-Identifier: Apache-2.0
// Host-only: c++ -std=c++17 -Iinclude test_voice_activity_detector.cpp -o /tmp/vad-test && /tmp/vad-test
#include "voice_activity_detector.hpp"

#include <array>
#include <cassert>
#include <cstdint>

using vibe_voice::VoiceActivityDetector;

struct Fixture {
    VoiceActivityDetector detector;
    bool speech_started = false;
    bool complete = false;
    bool no_speech = false;
    uint32_t processed_frames = 0;

    void feed(uint32_t milliseconds, int16_t alternating_level, int16_t dc_offset = 0)
    {
        std::array<int16_t, 256> pcm{};
        uint32_t frames_left = milliseconds * 16;
        while (frames_left > 0 && !complete && !no_speech) {
            const size_t frames = frames_left < pcm.size() ? frames_left : pcm.size();
            for (size_t i = 0; i < frames; ++i) {
                pcm[i] = static_cast<int16_t>(dc_offset + (i & 1 ? alternating_level : -alternating_level));
            }
            const auto event = detector.observe(pcm.data(), frames);
            if (event == VoiceActivityDetector::Event::SpeechStarted) speech_started = true;
            if (event == VoiceActivityDetector::Event::Complete) complete = true;
            if (event == VoiceActivityDetector::Event::NoSpeech) no_speech = true;
            frames_left -= static_cast<uint32_t>(frames);
            processed_frames += static_cast<uint32_t>(frames);
        }
    }
};

int main()
{
    {
        // Physical 1.85B readings: silence peaks around 11 while spoken
        // blocks peak around 137-166. Sustained speech must now be accepted.
        Fixture f;
        f.feed(300, 11);
        f.feed(400, 140);
        assert(f.speech_started && !f.complete);
        f.feed(1100, 11);
        assert(!f.complete);
        f.feed(200, 11);
        assert(f.complete && !f.no_speech);
    }
    {
        // Quiet office speech is much softer than the measured 140-160 level
        // normal voice, but still rises above the room's measured 11 floor.
        Fixture f;
        f.feed(300, 11);
        f.feed(350, 25);
        assert(f.speech_started && !f.complete);
        f.feed(1300, 11);
        assert(f.complete && !f.no_speech);
    }
    {
        // A quiet utterance can begin before a single background block was
        // captured. Speech has a changing envelope; constant noise does not.
        Fixture f;
        for (int16_t level : std::array<int16_t, 14>{
                 40, 55, 32, 60, 34, 54, 30, 58, 36, 52, 32, 56, 40, 50}) {
            f.feed(16, level);
        }
        assert(f.speech_started && !f.complete);
        f.feed(1300, 11);
        assert(f.complete);
    }
    {
        Fixture f;
        f.feed(8100, 40); // Constant first-block noise is not speech.
        assert(f.no_speech && !f.speech_started);
    }
    {
        Fixture f;
        f.feed(16, 40);
        f.feed(16, 1000);
        f.feed(8100, 40); // One click near BOOT cannot satisfy modulation span.
        assert(f.no_speech && !f.speech_started);
    }
    {
        Fixture f;
        f.feed(300, 30); // A noisier office adjusts the relative floor.
        f.feed(350, 60);
        assert(f.speech_started);
        f.feed(1300, 30);
        assert(f.complete);
    }
    {
        Fixture f;
        f.feed(8100, 30); // Sustained room noise is not an utterance.
        assert(f.no_speech && !f.speech_started);
    }
    {
        // Isolated loud blocks after speech must not restart the entire
        // 1.2-second end timer and delay upload for several seconds.
        Fixture f;
        f.feed(300, 11);
        f.feed(200, 80);
        assert(f.speech_started);
        for (int i = 0; i < 4; ++i) {
            f.feed(250, 11);
            f.feed(16, 1000);
        }
        f.feed(300, 11);
        assert(f.complete);
        assert(f.processed_frames < 2000 * 16);
    }
    {
        // Even several 64 ms noise bursts should only pause the end timer,
        // not postpone submission by many seconds.
        Fixture f;
        f.feed(300, 11);
        f.feed(200, 80);
        for (int i = 0; i < 4; ++i) {
            f.feed(250, 11);
            f.feed(64, 1000);
        }
        f.feed(300, 11);
        assert(f.complete);
        assert(f.processed_frames < 2200 * 16);
    }
    {
        // A genuine continuation lasting at least 80 ms restarts the pause.
        Fixture f;
        f.feed(300, 11);
        f.feed(200, 80);
        f.feed(900, 11);
        f.feed(80, 80);
        f.feed(500, 11);
        assert(!f.complete);
        f.feed(700, 11);
        assert(f.complete);
    }
    {
        Fixture f;
        f.feed(8100, 11); // Recorded quiet-room envelope must stay silent.
        assert(f.no_speech && !f.speech_started && !f.complete);
    }
    {
        Fixture f;
        f.feed(300, 11);
        f.feed(16, 160); // One click or tap is not an utterance.
        f.feed(8000, 11);
        assert(f.no_speech && !f.speech_started);
    }
    {
        Fixture f;
        f.feed(300, 100);
        f.feed(450, 2200);
        assert(f.speech_started);
        f.feed(1000, 100);
        assert(!f.complete);
        f.feed(300, 100);
        assert(f.complete && !f.no_speech);
    }
    {
        Fixture f;
        f.feed(8100, 80);
        assert(f.no_speech && !f.speech_started && !f.complete);
    }
    {
        Fixture f;
        f.feed(300, 100);
        f.feed(16, 3500); // A click or handling noise must not trigger upload.
        f.feed(8000, 100);
        assert(f.no_speech && !f.speech_started);
    }
    {
        Fixture f;
        f.feed(500, 2400); // Speaking immediately should survive calibration.
        assert(f.speech_started);
        f.feed(1300, 11);
        assert(f.complete);
    }
    {
        Fixture f;
        f.feed(500, 600); // Immediate medium-level speech must not become the noise floor.
        assert(f.speech_started);
        f.feed(1300, 11);
        assert(f.complete);
    }
    {
        Fixture f;
        f.feed(16, 140); // Immediate first syllable bootstraps the detector.
        f.feed(250, 55); // Softer follow-on speech should keep the onset.
        assert(f.speech_started);
        f.feed(1300, 11);
        assert(f.complete);
    }
    {
        Fixture f;
        f.feed(8100, 0, 1200); // Constant codec bias is not speech.
        assert(f.no_speech && !f.speech_started);
    }
    {
        Fixture f;
        f.feed(300, 40); // A noisier room raises the adaptive threshold.
        f.feed(500, 350);
        assert(f.speech_started);
        f.feed(500, 40); // A brief pause should not end the utterance.
        assert(!f.complete);
        f.feed(300, 350);
        f.feed(1300, 40);
        assert(f.complete);
    }
    {
        Fixture f;
        f.feed(30100, 2400); // Maximum recording length is a hard fallback.
        assert(f.complete && f.speech_started);
    }
}
