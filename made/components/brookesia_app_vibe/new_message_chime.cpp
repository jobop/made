#include "new_message_chime.hpp"

#include "board_voice_audio.h"
#include "vibe_theme.hpp"

#include <cstring>
#include <mutex>
#include "vibe_voice_capture.hpp"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const int16_t made_new_message_pcm[];
extern const unsigned made_new_message_pcm_samples;

namespace {
constexpr const char *kTag = "made_chime";
TaskHandle_t chime_task = nullptr;

// 主题提示音：/spiffs/theme_chime.wav（PCM 16bit，采样率/声道取自 WAV 头）。
struct ThemeClip {
    bool valid = false;
    uint8_t *data = nullptr;
    size_t size = 0;
    uint32_t rate = 16000;
    bool stereo = false;
};

bool loadThemeClip(ThemeClip &clip)
{
    clip = ThemeClip{};
    const std::string path = vibe_theme::soundPath();
    if (path.empty()) return false;
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length < 44 || length > 512 * 1024) {
        fclose(file);
        return false;
    }
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        fclose(file);
        return false;
    }
    if (fread(buffer, 1, length, file) != static_cast<size_t>(length)) {
        heap_caps_free(buffer);
        fclose(file);
        return false;
    }
    fclose(file);
    if (std::memcmp(buffer, "RIFF", 4) != 0 || std::memcmp(buffer + 8, "WAVE", 4) != 0) {
        heap_caps_free(buffer);
        return false;
    }
    size_t offset = 12;
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t data_offset = 0, data_size = 0;
    while (offset + 8 <= static_cast<size_t>(length)) {
        const char *id = reinterpret_cast<const char *>(buffer + offset);
        const uint32_t chunk_size = static_cast<uint32_t>(buffer[offset + 4]) |
                                    (static_cast<uint32_t>(buffer[offset + 5]) << 8) |
                                    (static_cast<uint32_t>(buffer[offset + 6]) << 16) |
                                    (static_cast<uint32_t>(buffer[offset + 7]) << 24);
        if (std::memcmp(id, "fmt ", 4) == 0 && offset + 8 + 16 <= static_cast<size_t>(length)) {
            const uint16_t format = static_cast<uint16_t>(buffer[offset + 8]) |
                                    (static_cast<uint16_t>(buffer[offset + 9]) << 8);
            channels = static_cast<uint16_t>(buffer[offset + 10]) |
                       (static_cast<uint16_t>(buffer[offset + 11]) << 8);
            rate = static_cast<uint32_t>(buffer[offset + 12]) |
                   (static_cast<uint32_t>(buffer[offset + 13]) << 8) |
                   (static_cast<uint32_t>(buffer[offset + 14]) << 16) |
                   (static_cast<uint32_t>(buffer[offset + 15]) << 24);
            bits = static_cast<uint16_t>(buffer[offset + 22]) |
                   (static_cast<uint16_t>(buffer[offset + 23]) << 8);
            if (format != 1 || bits != 16 || channels < 1 || channels > 2 ||
                rate < 8000 || rate > 48000) {
                heap_caps_free(buffer);
                return false;
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_offset = offset + 8;
            data_size = chunk_size > length - data_offset ? length - data_offset : chunk_size;
        }
        offset += 8 + chunk_size + (chunk_size & 1);
    }
    if (data_offset == 0 || data_size == 0) {
        heap_caps_free(buffer);
        return false;
    }
    clip.valid = true;
    clip.data = buffer;
    clip.size = data_size;
    clip.rate = rate;
    clip.stereo = channels == 2;
    return true;
}

bool playBuiltin()
{
    if (bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_VIBE) != ESP_OK) return false;
    bool played = false;
    if (bsp_extra_codec_set_fs(16000, 16, I2S_SLOT_MODE_STEREO) == ESP_OK) {
        played = true;
        size_t index = 0;
        while (index < made_new_message_pcm_samples) {
            int16_t stereo[128 * 2];
            const size_t frames = made_new_message_pcm_samples - index > 128 ? 128
                                                                             : made_new_message_pcm_samples - index;
            for (size_t i = 0; i < frames; ++i) {
                stereo[i * 2] = made_new_message_pcm[index + i];
                stereo[i * 2 + 1] = made_new_message_pcm[index + i];
            }
            size_t written = 0;
            const size_t bytes = frames * 2 * sizeof(int16_t);
            if (bsp_extra_i2s_write(stereo, bytes, &written, 200) != ESP_OK || written != bytes) {
                played = false;
                break;
            }
            index += frames;
        }
    }
    (void)bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_VIBE);
    return played;
}

bool playClip()
{
    if (bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_VIBE) != ESP_OK) return false;
    bool played = false;
    ThemeClip clip;
    const bool theme_sound = loadThemeClip(clip);
    if (theme_sound && bsp_extra_codec_set_fs(clip.rate, 16, clip.stereo ? I2S_SLOT_MODE_STEREO
                                                                        : I2S_SLOT_MODE_MONO) == ESP_OK) {
        played = true;
        const size_t frame_channels = clip.stereo ? 2 : 1;
        size_t offset = 0;
        while (offset < clip.size) {
            int16_t block[128 * 2];
            const size_t frames = (clip.size - offset) / (frame_channels * sizeof(int16_t));
            const size_t chunk_frames = frames > 128 ? 128 : frames;
            if (chunk_frames == 0) break;
            if (clip.stereo) {
                std::memcpy(block, clip.data + offset, chunk_frames * frame_channels * sizeof(int16_t));
            } else {
                const auto *mono = reinterpret_cast<const int16_t *>(clip.data + offset);
                for (size_t i = 0; i < chunk_frames; ++i) {
                    block[i * 2] = mono[i];
                    block[i * 2 + 1] = mono[i];
                }
            }
            size_t written = 0;
            const size_t bytes = chunk_frames * 2 * sizeof(int16_t);
            if (bsp_extra_i2s_write(block, bytes, &written, 200) != ESP_OK || written != bytes) {
                played = false;
                break;
            }
            offset += chunk_frames * frame_channels * sizeof(int16_t);
        }
    } else if (!theme_sound &&
               bsp_extra_codec_set_fs(16000, 16, I2S_SLOT_MODE_STEREO) == ESP_OK) {
        played = true;
        size_t index = 0;
        while (index < made_new_message_pcm_samples) {
            int16_t stereo[128 * 2];
            const size_t frames = made_new_message_pcm_samples - index > 128 ? 128
                                                                             : made_new_message_pcm_samples - index;
            for (size_t i = 0; i < frames; ++i) {
                stereo[i * 2] = made_new_message_pcm[index + i];
                stereo[i * 2 + 1] = made_new_message_pcm[index + i];
            }
            size_t written = 0;
            const size_t bytes = frames * 2 * sizeof(int16_t);
            if (bsp_extra_i2s_write(stereo, bytes, &written, 200) != ESP_OK || written != bytes) {
                played = false;
                break;
            }
            index += frames;
        }
    }
    if (theme_sound && clip.data != nullptr) heap_caps_free(clip.data);
    (void)bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_VIBE);
    return played;
}

constexpr uint32_t kChimeMessage = 1;
constexpr uint32_t kChimePreview = 2;
constexpr uint32_t kChimeWav = 4;
std::mutex wav_mu;
uint8_t *pending_wav = nullptr;
size_t pending_wav_size = 0;

bool playMemoryWav(const uint8_t *buffer, size_t length)
{
    if (!buffer || length < 44 || std::memcmp(buffer, "RIFF", 4) != 0 || std::memcmp(buffer + 8, "WAVE", 4) != 0) return false;
    size_t offset = 12;
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t data_offset = 0, data_size = 0;
    while (offset + 8 <= length) {
        const char *id = reinterpret_cast<const char *>(buffer + offset);
        const uint32_t chunk_size = static_cast<uint32_t>(buffer[offset + 4]) |
                                    (static_cast<uint32_t>(buffer[offset + 5]) << 8) |
                                    (static_cast<uint32_t>(buffer[offset + 6]) << 16) |
                                    (static_cast<uint32_t>(buffer[offset + 7]) << 24);
        if (std::memcmp(id, "fmt ", 4) == 0 && offset + 8 + 16 <= length) {
            const uint16_t format = static_cast<uint16_t>(buffer[offset + 8]) |
                                    (static_cast<uint16_t>(buffer[offset + 9]) << 8);
            channels = static_cast<uint16_t>(buffer[offset + 10]) |
                       (static_cast<uint16_t>(buffer[offset + 11]) << 8);
            rate = static_cast<uint32_t>(buffer[offset + 12]) |
                   (static_cast<uint32_t>(buffer[offset + 13]) << 8) |
                   (static_cast<uint32_t>(buffer[offset + 14]) << 16) |
                   (static_cast<uint32_t>(buffer[offset + 15]) << 24);
            bits = static_cast<uint16_t>(buffer[offset + 22]) |
                   (static_cast<uint16_t>(buffer[offset + 23]) << 8);
            if (format != 1 || bits != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 48000) return false;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_offset = offset + 8;
            data_size = chunk_size > length - data_offset ? length - data_offset : chunk_size;
        }
        const size_t step = 8u + chunk_size + (chunk_size & 1u);
        if (offset + step < offset || offset + step > length + 8) return false;
        offset += step;
    }
    if (data_offset == 0 || data_size < sizeof(int16_t)) return false;
    if (bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_VIBE) != ESP_OK) return false;
    bool played = false;
    const bool stereo = channels == 2;
    if (bsp_extra_codec_set_fs(rate, 16, stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO) == ESP_OK) {
        played = true;
        const size_t frame_channels = stereo ? 2 : 1;
        size_t cursor = 0;
        while (cursor < data_size) {
            int16_t block[128 * 2];
            const size_t frames = (data_size - cursor) / (frame_channels * sizeof(int16_t));
            const size_t chunk_frames = frames > 128 ? 128 : frames;
            if (chunk_frames == 0) break;
            if (stereo) {
                std::memcpy(block, buffer + data_offset + cursor, chunk_frames * frame_channels * sizeof(int16_t));
            } else {
                const auto *mono = reinterpret_cast<const int16_t *>(buffer + data_offset + cursor);
                for (size_t i = 0; i < chunk_frames; ++i) {
                    block[i * 2] = mono[i];
                    block[i * 2 + 1] = mono[i];
                }
            }
            size_t written = 0;
            const size_t bytes = chunk_frames * 2 * sizeof(int16_t);
            if (bsp_extra_i2s_write(block, bytes, &written, 200) != ESP_OK || written != bytes) {
                played = false;
                break;
            }
            cursor += chunk_frames * frame_channels * sizeof(int16_t);
        }
    }
    (void)bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_VIBE);
    return played;
}

void chimeEntry(void *);

bool ensureChimeTask()
{
    if (chime_task) return true;
    if (xTaskCreate(chimeEntry, "made_chime", 8192, nullptr, 3, &chime_task) != pdPASS) {
        chime_task = nullptr;
        ESP_LOGW(kTag, "Cannot start the new-message prompt");
        return false;
    }
    return true;
}

void chimeEntry(void *)
{
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        if (bits & kChimeWav) {
            uint8_t *data = nullptr;
            size_t size = 0;
            {
                std::lock_guard<std::mutex> lock(wav_mu);
                data = pending_wav;
                size = pending_wav_size;
                pending_wav = nullptr;
                pending_wav_size = 0;
            }
            if (data) {
                bool played = false;
                for (int attempt = 0; attempt < 15 && !played; ++attempt) {
                    const auto phase = vibe_voice::status().phase;
                    if (phase == vibe_voice::Phase::Recording || phase == vibe_voice::Phase::Uploading) {
                        vTaskDelay(pdMS_TO_TICKS(400));
                        continue;
                    }
                    played = playMemoryWav(data, size);
                    if (!played) vTaskDelay(pdMS_TO_TICKS(400));
                }
                heap_caps_free(data);
                if (!played) ESP_LOGW(kTag, "Could not play the commanded voice");
            }
        }
        if ((bits & (kChimeMessage | kChimePreview)) == 0) continue;
        const bool preview = (bits & kChimeMessage) == 0 && (bits & kChimePreview) != 0;
        bool played = false;
        for (int attempt = 0; attempt < 15 && !played; ++attempt) {
            const auto phase = vibe_voice::status().phase;
            if (phase == vibe_voice::Phase::Recording || phase == vibe_voice::Phase::Uploading) {
                vTaskDelay(pdMS_TO_TICKS(400));
                continue;
            }
            played = preview ? playBuiltin() : playClip();
            if (!played) vTaskDelay(pdMS_TO_TICKS(400));
        }
        if (!played) ESP_LOGW(kTag, "Could not play the new-message prompt");
    }
}
} // namespace

void made_chime_new_message()
{
    if (!ensureChimeTask()) return;
    xTaskNotify(chime_task, kChimeMessage, eSetBits);
}

void made_chime_preview()
{
    if (!ensureChimeTask()) return;
    xTaskNotify(chime_task, kChimePreview, eSetBits);
}

bool made_chime_play_wav(const uint8_t *data, size_t size)
{
    if (!data || size < 44 || size > 256 * 1024 || !ensureChimeTask()) return false;
    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) copy = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    if (copy == nullptr) return false;
    std::memcpy(copy, data, size);
    uint8_t *previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(wav_mu);
        previous = pending_wav;
        pending_wav = copy;
        pending_wav_size = size;
    }
    if (previous) heap_caps_free(previous);
    xTaskNotify(chime_task, kChimeWav, eSetBits);
    return true;
}
