#ifndef EIDOLON_AGENT_SESSION_TRACKER_H_
#define EIDOLON_AGENT_SESSION_TRACKER_H_

#include <string>

#include "eidolon_ui_types.h"

namespace eidolon {

class AgentSessionTracker {
public:
    void OnTranscription(const TranscriptionEvent& event);
    void OnAgentPhase(AgentPhase phase);
    void OnRoomConnected();
    void OnRoomDisconnected();

    AgentPhase GetPhase() const { return phase_; }
    const std::string& LastTranscription() const { return last_transcription_; }
    TranscriptionSource LastTranscriptionSource() const { return last_transcription_source_; }

private:
    AgentPhase phase_ = AgentPhase::Silent;
    std::string last_transcription_;
    TranscriptionSource last_transcription_source_ = TranscriptionSource::Unknown;
};

}  // namespace eidolon

#endif  // EIDOLON_AGENT_SESSION_TRACKER_H_
