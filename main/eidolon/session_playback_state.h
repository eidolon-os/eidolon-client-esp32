#ifndef EIDOLON_SESSION_PLAYBACK_STATE_H_
#define EIDOLON_SESSION_PLAYBACK_STATE_H_

#include <cstdint>
#include <cstring>
#include <optional>

namespace eidolon {

// UI listening/user-VAD events must not release the independent playback fence.
// The authenticated Channel agent's own state is the source of this signal.
inline std::optional<bool> SessionPlaybackSignal(const char* state, const char* reason)
{
    if (!state || !reason || std::strncmp(reason, "agent_state:", 12) != 0) {
        return std::nullopt;
    }
    if (std::strcmp(state, "speaking") == 0) return true;
    if (std::strcmp(state, "listening") == 0 || std::strcmp(state, "thinking") == 0) {
        return false;
    }
    return std::nullopt;
}

class SessionPlaybackState {
public:
    // Same drain/echo window as the board's existing local playback detection.
    static constexpr int64_t kTailUs = 1200 * 1000;
    void SetSpeaking(bool speaking, int64_t now_us)
    {
        if (speaking_ && !speaking) release_at_us_ = now_us + kTailUs;
        speaking_ = speaking;
    }
    bool Active(int64_t now_us) const
    {
        return speaking_ || now_us < release_at_us_;
    }
    void Reset() { speaking_ = false; release_at_us_ = 0; }
private:
    bool speaking_ = false;
    int64_t release_at_us_ = 0;
};

}  // namespace eidolon
#endif
