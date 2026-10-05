#include "new_message_chime.hpp"

#include "board_voice_audio.h"
#include "vibe_voice_capture.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const int16_t made_new_message_pcm[];
extern const unsigned made_new_message_pcm_samples;

namespace {
constexpr const char *kTag = "made_chime";
TaskHandle_t chime_task = nullptr;

bool playClip()
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

void chimeEntry(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool played = false;
        for (int attempt = 0; attempt < 15 && !played; ++attempt) {
            const auto phase = vibe_voice::status().phase;
            if (phase == vibe_voice::Phase::Recording || phase == vibe_voice::Phase::Uploading) {
                vTaskDelay(pdMS_TO_TICKS(400));
                continue;
            }
            played = playClip();
            if (!played) vTaskDelay(pdMS_TO_TICKS(400));
        }
        if (!played) ESP_LOGW(kTag, "Could not play the new-message prompt");
    }
}
} // namespace

void made_chime_new_message()
{
    if (!chime_task && xTaskCreate(chimeEntry, "made_chime", 8192, nullptr, 3, &chime_task) != pdPASS) {
        chime_task = nullptr;
        ESP_LOGW(kTag, "Cannot start the new-message prompt");
        return;
    }
    xTaskNotifyGive(chime_task);
}
