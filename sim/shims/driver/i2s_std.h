/* sim/shims/driver/i2s_std.h — host 桩：仅为 board_voice_audio.h 头文件编译。 */
#pragma once

typedef enum {
    I2S_SLOT_MODE_MONO = 0,
    I2S_SLOT_MODE_STEREO,
} i2s_slot_mode_t;
