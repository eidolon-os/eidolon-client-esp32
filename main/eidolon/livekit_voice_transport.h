#ifndef EIDOLON_LIVEKIT_VOICE_TRANSPORT_H_
#define EIDOLON_LIVEKIT_VOICE_TRANSPORT_H_

#include <atomic>
#include <memory>

#include "eidolon_device_store.h"
#include "eidolon_voice_controller.h"
#include "voice_session_transport.h"
#include "voice_runtime_projector.h"

namespace eidolon {

class GuardService;

class LiveKitVoiceTransport : public IVoiceSessionTransport {
public:
    explicit LiveKitVoiceTransport(VoiceSessionCallbacks cb, GuardService* guard_service = nullptr);
    ~LiveKitVoiceTransport() override;

    void OnActivationComplete() override;
    void OnNetworkLost() override;
    void OnNetworkRestored() override;
    void QuiesceForCommissioning(
        std::function<void(bool)> completion) override;
    void OnAmbientPresenceChanged(bool present) override;

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
    VoiceRuntimeStatus BuildRuntimeStatus(VoiceSessionState state) const;
    void NotifyRuntimeStatus(VoiceSessionState state);

    std::unique_ptr<EidolonVoiceController> controller_;
    EidolonDeviceStore device_store_;
    std::function<void(const VoiceRuntimeStatus&)> on_runtime_status_;
    std::atomic<bool> operational_ready_{false};
    std::atomic<bool> service_was_ready_{false};
    std::atomic<bool> mic_enabled_{true};
};

}  // namespace eidolon

#endif  // EIDOLON_LIVEKIT_VOICE_TRANSPORT_H_
