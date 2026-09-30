# ESP32-S3-Touch-LCD-1.85B voice audio

This component owns the board's shared I2S peripheral while a voice app is open.
It uses the 1.85B BSP pin definitions, ES8311 for playback, and ES7210 for capture.
The original Brookesia XiaoZhi app and the separate Vibe coding app use one session
owner lock, but their speech recognition and network protocols remain independent.

The capture format is signed 16-bit little-endian, four interleaved TDM slots at
the configured sample rate. XiaoZhi uses 24 kHz. Vibe requests 16 kHz and
selects the stronger physical microphone from slots 0 and 2 for a mono WAV;
slot 1 is the echo reference. The slot order and mic gain
are inherited from the adjacent 1.75-inch Waveshare Brookesia implementation;
they must be checked on a physical 1.85B board before claiming recording quality.
The ES8311 playback channel uses two 32-bit slots containing 16 valid audio bits
so its 64-clock frame matches the ES7210's four 16-bit capture slots on the
shared full-duplex BCLK/WS lines. Actual clock and microphone timing still need
confirmation on the physical board.

Call sequence for an app:

1. `bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_XIAOZHI)` or
   `BSP_EXTRA_AUDIO_OWNER_VIBE`. A busy return means another app owns audio.
2. `bsp_extra_codec_set_voice_fs(rate, 16, 4, 0x0F, 0x07)`.
3. Read with `bsp_extra_i2s_read()`, and write playback with
   `bsp_extra_i2s_write()` if required.
4. Stop worker tasks, call `bsp_extra_codec_dev_stop()`, then
   `bsp_extra_audio_session_release(owner)`. Release deletes the codec and I2S
   handles so the other app can acquire the peripheral.

The original 1.85B BSP audio helpers allocate the same I2S port without this
owner lock. Other apps must not call those helpers while a voice session is open.
The adapter returns a recoverable error if the port is already occupied. Display,
touch, Wi-Fi, and the other Brookesia apps do not initialize audio in the current
desktop project.

The XiaoZhi app's ten source files and this audio source passed an ESP-IDF 5.5
Xtensa compiler syntax check. A full firmware build and physical 1.85B flash,
mic slot test, speaker test, wake phrase test, and session switching test are
required before hardware validation is complete. The WakeNet `model` partition
must be flashed with `srmodels.bin`; an app-only flash is insufficient.
