#ifndef EIDOLON_VOICE_SESSION_TRANSPORT_H_
#define EIDOLON_VOICE_SESSION_TRANSPORT_H_

#include <esp_err.h>
#include <functional>
#include <memory>
#include <string>

#include "eidolon_voice_controller.h"

namespace eidolon {

class GuardService;

struct VoiceSessionCallbacks {
    std::function<void(VoiceSessionState)> on_session_state;
    std::function<void(const TranscriptionEvent&)> on_transcription;
    std::function<void(AgentPhase)> on_agent_phase;
    std::function<void(const std::string& outcome)> on_ptt_turn_status;
    std::function<void(const std::string&)> on_error;
};

class IVoiceSessionTransport {
public:
    virtual ~IVoiceSessionTransport() = default;

    virtual void OnActivationComplete() = 0;
    virtual void OnNetworkLost() = 0;
    virtual void OnNetworkRestored() = 0;

    virtual void ToggleSession() = 0;
    virtual void JoinSession() = 0;
    virtual void LeaveSession() = 0;
    virtual bool IsInSession() const = 0;

    // Push-to-talk (hold-to-talk): press opens the mic (joining the room first if
    // needed); release closes it and signals end-of-turn. No-ops outside PTT mode.
    virtual void PttPress() = 0;
    virtual void PttRelease() = 0;

    virtual void SetMicrophoneEnabled(bool enabled) = 0;
    virtual bool IsMicrophoneEnabled() const = 0;

    virtual VoiceSessionState GetSessionState() const = 0;
    // Why the last voice session ended (EndReason::None until the channel reports
    // one). Lets the UI distinguish a normal end of conversation from a join
    // failure.
    virtual EndReason LastEndReason() const = 0;
};

std::unique_ptr<IVoiceSessionTransport> CreateLiveKitVoiceTransport(
    VoiceSessionCallbacks cb, GuardService* guard_service = nullptr);

}  // namespace eidolon

#endif  // EIDOLON_VOICE_SESSION_TRANSPORT_H_
