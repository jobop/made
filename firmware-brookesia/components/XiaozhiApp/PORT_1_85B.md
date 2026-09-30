# XiaoZhi AIChats on ESP32-S3-Touch-LCD-1.85B

This desktop application is adapted from Waveshare's open source 1.75-inch
Brookesia AIChats app. It remains a separate app from Vibe Coding. XiaoZhi uses
its own xiaozhi.me streaming ASR, conversation, activation, and MCP code; Vibe
records a separate WAV and sends it to the computer bridge for transcription.

Both apps use the 1.85B ES7210 microphones and ES8311 speaker through the
shared `board_voice_audio` hardware component. Its session owner lock allows
only one app to use the I2S port at a time. Each app must stop audio workers and
release the session when closed. The 1.85B audio pins and codec addresses were
checked against the Waveshare 1.85B BSP and the board's XiaoZhi configuration.
The ES7210 TDM slot order is inherited from the adjacent 1.75-inch implementation
and still needs a recording check on the actual 1.85B board.

The partition table includes a 960 KB `model` partition for the ESP-SR WakeNet
model. `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y` selects the wake phrase; ESP-SR
generates `build/srmodels/srmodels.bin` and includes it in `idf.py flash`. The
4 MB `assets` SPIFFS partition contains the complete XiaoZhi Chinese font
under `/spiffs/xiaozhi/font.bin`. The main program mounts `assets` on startup.
Use a full `idf.py flash` after changing the partition table or model selection:
flashing only the app binary leaves the model and font unavailable. A newly
built firmware may also need activation in the user's xiaozhi.me account because
device identity and NVS data may differ from the previous firmware.

The XiaoZhi app's ten source files and the board audio source passed an
ESP-IDF 5.5 Xtensa compiler syntax check. The ten-app firmware link/build is
being validated separately. No firmware from this port has been flashed to a
physical 1.85B board yet, so microphone mapping, speaker output, wake phrase,
activation, touch layout, and switching between XiaoZhi and Vibe remain
unverified on hardware.
