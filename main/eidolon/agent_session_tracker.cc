#include "agent_session_tracker.h"

namespace eidolon {

void AgentSessionTracker::OnTranscription(const std::string& text)
{
    if (text.empty()) {
        return;
    }
    last_transcription_ = text;
    phase_ = AgentPhase::UserSpeaking;
}

void AgentSessionTracker::OnRoomConnected()
{
    phase_ = AgentPhase::Silent;
    last_transcription_.clear();
}

void AgentSessionTracker::OnRoomDisconnected()
{
    phase_ = AgentPhase::Silent;
    last_transcription_.clear();
}

}  // namespace eidolon
