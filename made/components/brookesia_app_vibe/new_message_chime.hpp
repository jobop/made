#pragma once

// Ask the speaker to say “新消息”. Playback runs off the UI thread and
// waits if the microphone currently owns the codec.
void made_chime_new_message();
// 音量调节时播放内置短提示，不读主题里的长 wav。
void made_chime_preview();
