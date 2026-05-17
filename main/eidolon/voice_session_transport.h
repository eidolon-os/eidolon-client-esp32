#ifndef EIDOLON_VOICE_SESSION_TRANSPORT_H_
#define EIDOLON_VOICE_SESSION_TRANSPORT_H_

#include <esp_err.h>
#include <functional>
#include <memory>
#include <string>

#include "eidolon_voice_controller.h"

namespace eidolon {

struct VoiceSessionCallbacks {
    std::function<void(VoiceSessionState)> on_session_state;
    std::function<void(const std::string&)> on_transcription;
    std::function<void(const std::string&)> on_error;
};

class IVoiceSessionTransport {
public:
    virtual ~IVoiceSessionTransport() = default;

    virtual void OnActivationComplete() = 0;
    virtual void OnNetworkLost() = 0;

    virtual void ToggleSession() = 0;
    virtual void JoinSession() = 0;
    virtual void LeaveSession() = 0;
    virtual bool IsInSession() const = 0;

    virtual void SetMicrophoneEnabled(bool enabled) = 0;
    virtual bool IsMicrophoneEnabled() const = 0;

    virtual VoiceSessionState GetSessionState() const = 0;
};

std::unique_ptr<IVoiceSessionTransport> CreateLiveKitVoiceTransport(VoiceSessionCallbacks cb);

}  // namespace eidolon

#endif  // EIDOLON_VOICE_SESSION_TRANSPORT_H_
