#pragma once

// Ask the speaker to say “新消息”. Playback runs off the UI thread and
// waits if the microphone currently owns the codec.
void made_chime_new_message();
