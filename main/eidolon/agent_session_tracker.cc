#include "agent_session_tracker.h"

namespace eidolon {

void AgentSessionTracker::OnTranscription(const TranscriptionEvent& event)
{
    if (event.text.empty()) {
        return;
    }
    last_transcription_ = event.text;
    last_transcription_source_ = event.source;

    switch (event.source) {
    case TranscriptionSource::User:
        phase_ = AgentPhase::UserSpeaking;
        break;
    case TranscriptionSource::Agent:
        phase_ = AgentPhase::AgentSpeaking;
        break;
    case TranscriptionSource::System:
    case TranscriptionSource::Unknown:
    default:
        break;
    }
}

void AgentSessionTracker::OnAgentPhase(AgentPhase phase)
{
    phase_ = phase;
}

void AgentSessionTracker::OnRoomConnected()
{
    phase_ = AgentPhase::Silent;
    last_transcription_.clear();
    last_transcription_source_ = TranscriptionSource::Unknown;
}

void AgentSessionTracker::OnRoomDisconnected()
{
    phase_ = AgentPhase::Silent;
    last_transcription_.clear();
    last_transcription_source_ = TranscriptionSource::Unknown;
}

}  // namespace eidolon
