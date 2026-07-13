#ifndef EIDOLON_LIVEKIT_VOICE_TRANSPORT_H_
#define EIDOLON_LIVEKIT_VOICE_TRANSPORT_H_

#include <memory>

#include "eidolon_device_store.h"
#include "eidolon_voice_controller.h"
#include "voice_session_transport.h"

namespace eidolon {

class GuardService;

class LiveKitVoiceTransport : public IVoiceSessionTransport {
public:
    explicit LiveKitVoiceTransport(VoiceSessionCallbacks cb, GuardService* guard_service = nullptr);
    ~LiveKitVoiceTransport() override;

    void OnActivationComplete() override;
    void OnNetworkLost() override;
    void OnNetworkRestored() override;

    void ToggleSession() override;
    void JoinSession() override;
    void LeaveSession() override;
    bool IsInSession() const override;

    void PttPress() override;
    void PttRelease() override;

    void SetMicrophoneEnabled(bool enabled) override;
    bool IsMicrophoneEnabled() const override;

    VoiceSessionState GetSessionState() const override;
    EndReason LastEndReason() const override;

    EidolonVoiceController* controller() { return controller_.get(); }

private:
    std::unique_ptr<EidolonVoiceController> controller_;
    EidolonDeviceStore device_store_;
    bool mic_enabled_ = true;
};

}  // namespace eidolon

#endif  // EIDOLON_LIVEKIT_VOICE_TRANSPORT_H_
