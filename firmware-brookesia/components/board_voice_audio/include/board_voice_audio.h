#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ES7210 serializes MIC1, an echo-reference input, MIC2 and MIC4 in this order.
// The first three physical inputs match the dual-microphone board schematic.
#define CODEC_VOICE_INPUT_CHANNELS 4
#define BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK 0x0FU
#define BSP_EXTRA_ES7210_PHYSICAL_CONNECTED_MIC_MASK 0x07U

typedef enum {
    BSP_EXTRA_AUDIO_OWNER_NONE = 0,
    BSP_EXTRA_AUDIO_OWNER_XIAOZHI = 1,
    BSP_EXTRA_AUDIO_OWNER_VIBE = 2,
    BSP_EXTRA_AUDIO_OWNER_MAX,
} bsp_extra_audio_owner_t;

esp_err_t bsp_extra_audio_session_acquire(bsp_extra_audio_owner_t owner);
esp_err_t bsp_extra_audio_session_release(bsp_extra_audio_owner_t owner);
bsp_extra_audio_owner_t bsp_extra_audio_session_get_owner(void);
const char* bsp_extra_audio_owner_name(bsp_extra_audio_owner_t owner);

esp_err_t bsp_extra_codec_set_voice_fs(uint32_t rate, uint32_t bits,
                                        uint8_t record_channels,
                                        uint16_t record_tdm_slot_mask,
                                        uint16_t record_mic_gain_mask);
esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t channels);
esp_err_t bsp_extra_codec_dev_stop(void);
esp_err_t bsp_extra_codec_mute_set(bool muted);
esp_err_t bsp_extra_i2s_read(void* buffer, size_t length, size_t* bytes_read, uint32_t timeout_ms);
esp_err_t bsp_extra_i2s_write(void* buffer, size_t length, size_t* bytes_written, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
