#pragma once

#include <cstddef>
#include <cstdint>

// Ask the speaker to say “新消息”. Playback runs off the UI thread and
// waits if the microphone currently owns the codec.
void made_chime_new_message();
// 音量调节时播放内置短提示，不读主题里的长 wav。
void made_chime_preview();
// 播放一段随命令下发的 PCM WAV。复制到 PSRAM 后立即返回，播放在提示音任务里进行。
bool made_chime_play_wav(const uint8_t *data, size_t size);
