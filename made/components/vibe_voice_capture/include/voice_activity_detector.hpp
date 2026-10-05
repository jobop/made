// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace vibe_voice {

// Small, allocation-free voice endpoint detector for 16 kHz mono PCM.
// This decides when to submit an utterance; transcription still runs on the PC.
class VoiceActivityDetector {
public:
    enum class Event { None, SpeechStarted, Complete, NoSpeech };

    static constexpr uint32_t sample_rate = 16000;
    static constexpr uint32_t voice_onset_samples = sample_rate / 8;        // 125 ms
    static constexpr uint32_t voice_resume_samples = sample_rate * 80 / 1000; // 80 ms
    static constexpr uint32_t bootstrap_window_samples = sample_rate / 2; // 500 ms
    static constexpr uint32_t end_silence_samples = sample_rate * 6 / 5;    // 1.2 s
    static constexpr uint32_t no_speech_samples = sample_rate * 8;         // 8 s
    static constexpr uint32_t max_samples = sample_rate * 30;              // 30 s

    Event observe(const int16_t *pcm, size_t frames)
    {
        if (pcm == nullptr || frames == 0) return Event::None;

        // Remove each block's DC bias before measuring its envelope. ES7210
        // samples are signed PCM; 64-bit sums keep the calculation bounded.
        int64_t sum = 0;
        for (size_t i = 0; i < frames; ++i) sum += pcm[i];
        const int32_t mean = static_cast<int32_t>(sum / static_cast<int64_t>(frames));
        uint64_t deviation = 0;
        for (size_t i = 0; i < frames; ++i) {
            const int32_t centered = static_cast<int32_t>(pcm[i]) - mean;
            deviation += centered < 0 ? -centered : centered;
        }
        const uint32_t level = static_cast<uint32_t>(deviation / frames);
        elapsed_samples_ += static_cast<uint32_t>(frames);

        // If the user begins speaking in the very first block, that block may
        // look like a high noise floor. Constant room noise and one short tap
        // cannot be distinguished by amplitude alone, so watch the first
        // 500 ms for several envelope changes spanning at least 125 ms.
        if (bootstrap_reference_level_ == 0) {
            bootstrap_reference_level_ = level;
            bootstrap_previous_level_ = level;
            bootstrap_min_level_ = level;
        } else if (!speech_started_ && elapsed_samples_ <= bootstrap_window_samples &&
                   bootstrap_reference_level_ >= 22 && bootstrap_reference_level_ <= 60) {
            bootstrap_min_level_ = std::min(bootstrap_min_level_, level);
            const uint32_t difference = level > bootstrap_previous_level_ ?
                level - bootstrap_previous_level_ : bootstrap_previous_level_ - level;
            const uint32_t change_threshold = std::max<uint32_t>(7, bootstrap_reference_level_ / 5);
            if (difference >= change_threshold) {
                if (bootstrap_changes_++ == 0) bootstrap_first_change_samples_ = elapsed_samples_;
                bootstrap_last_change_samples_ = elapsed_samples_;
            }
            bootstrap_previous_level_ = level;
        }

        // On the 1.85B, a quiet room is near 11 while low-volume speech can
        // fall below the old ~49 threshold. Bootstrap from only the first
        // block so speech immediately after pressing BOOT is not absorbed into
        // a 200 ms calibration window. Sustained louder room noise still
        // raises the threshold relative to its measured floor.
        const uint32_t threshold = speech_threshold_override_ != 0 ? speech_threshold_override_ :
            noise_observations_ == 0 && onset_samples_ == 0 ? 100 :
            std::max<uint32_t>(22, noise_floor_ * 3 / 2 + 5);
        const bool loud = level > threshold;
        if (!loud && !speech_started_) {
            noise_floor_ = noise_observations_ == 0 ? level :
                (noise_floor_ * 7 + level) / 8;
            ++noise_observations_;
        }

        if (!speech_started_) {
            onset_samples_ = loud ? onset_samples_ + static_cast<uint32_t>(frames) : 0;
            const bool immediate_soft_speech = elapsed_samples_ <= bootstrap_window_samples &&
                bootstrap_changes_ >= 3 &&
                bootstrap_last_change_samples_ - bootstrap_first_change_samples_ >= voice_onset_samples &&
                elapsed_samples_ - bootstrap_last_change_samples_ <= voice_resume_samples &&
                level > 22;
            if (onset_samples_ >= voice_onset_samples || immediate_soft_speech) {
                speech_started_ = true;
                if (immediate_soft_speech && onset_samples_ < voice_onset_samples) {
                    speech_threshold_override_ = std::max<uint32_t>(22, bootstrap_min_level_ / 2 + 10);
                }
                return Event::SpeechStarted;
            }
        } else if (loud) {
            resume_samples_ += static_cast<uint32_t>(frames);
            // A single loud I2S block from a tap must not postpone upload by
            // another 1.2 seconds; only a short run of speech restarts the
            // end-of-utterance timer.
            if (resume_samples_ >= voice_resume_samples) quiet_samples_ = 0;
        } else {
            resume_samples_ = 0;
            quiet_samples_ += static_cast<uint32_t>(frames);
        }
        if (speech_started_ && quiet_samples_ >= end_silence_samples) return Event::Complete;
        if (elapsed_samples_ >= max_samples) return speech_started_ ? Event::Complete : Event::NoSpeech;
        if (!speech_started_ && elapsed_samples_ >= no_speech_samples) return Event::NoSpeech;
        return Event::None;
    }

    bool heardSpeech() const { return speech_started_; }
    uint32_t noiseFloor() const { return noise_floor_; }

private:
    uint32_t elapsed_samples_ = 0;
    uint32_t noise_floor_ = 0;
    uint32_t noise_observations_ = 0;
    uint32_t onset_samples_ = 0;
    uint32_t resume_samples_ = 0;
    uint32_t quiet_samples_ = 0;
    uint32_t bootstrap_reference_level_ = 0;
    uint32_t bootstrap_previous_level_ = 0;
    uint32_t bootstrap_min_level_ = 0;
    uint32_t bootstrap_changes_ = 0;
    uint32_t bootstrap_first_change_samples_ = 0;
    uint32_t bootstrap_last_change_samples_ = 0;
    uint32_t speech_threshold_override_ = 0;
    bool speech_started_ = false;
};

} // namespace vibe_voice
