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
};

// Selects the stronger of the two physical microphones independently of
// XiaoZhi, then creates a task on the desktop bridge.
// A BOOT single click starts recording; speech end automatically uploads it.
// Silence before any speech times out without upload; recording is capped at 30 seconds.
bool start(const std::string& bridge_url, const std::string& token,
           const std::string& provider, const std::string& project_id,
           const std::string& session_id);
void cancel();
Status status();

}  // namespace vibe_voice
