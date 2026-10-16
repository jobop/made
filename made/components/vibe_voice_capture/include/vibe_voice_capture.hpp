#pragma once

#include <string>

namespace vibe_voice {

enum class Phase {
    Idle,
    Recording,
    Uploading,
    Submitted,
    Error,
};

struct Status {
    Phase phase;
    std::string message;
    std::string task_id; // Bridge's new task ID after a successful 201 response.
    bool hold_mode;      // true = 长按录音（松手结束）；false = VAD 静音自动结束。
};

// Selects the stronger of the two physical microphones independently of
// XiaoZhi, then creates a task on the desktop bridge.
// 长按模式（hold=true）：BOOT 按住开始录音、松手（finish()）立即上传，
// 静音不自动结束。VAD 模式（hold=false，远程 voice.start 兜底）：说话结束
// 自动上传。两种模式录音都封顶 30 秒。
bool start(const std::string& bridge_url, const std::string& token,
           const std::string& provider, const std::string& project_id,
           const std::string& session_id, bool hold_mode = false);
// 长按模式的松手：结束录音并立即上传（仅 Recording 阶段有效）。
void finish();
void cancel();
Status status();

}  // namespace vibe_voice
