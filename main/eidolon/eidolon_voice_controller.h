#ifndef EIDOLON_VOICE_CONTROLLER_H_
#define EIDOLON_VOICE_CONTROLLER_H_

#include <esp_err.h>
#include <functional>

#include "hub_types.h"
#include "livekit_session.h"

namespace eidolon {

enum class VoiceSessionState {
    Idle,
    ConfigReady,
    Connecting,
    InRoom,
    Reconnecting,
    Error,
};

class EidolonVoiceController {
public:
    using StateCallback = std::function<void(VoiceSessionState)>;

    void OnHubActivationSucceeded();
    void OnNetworkLost();

    esp_err_t JoinRoom();
    esp_err_t LeaveRoom();
    esp_err_t SetMicEnabled(bool enabled);

    VoiceSessionState GetState() const { return state_; }
    void SetOnStateChanged(StateCallback cb) { on_state_changed_ = std::move(cb); }
    void SetOnTranscription(std::function<void(const std::string&)> cb);

private:
    esp_err_t LoadStoredConfig();
    esp_err_t RefreshHubConfig();
    void SetState(VoiceSessionState state);
    void OnLiveKitState(LiveKitConnectionState lk_state);

    LiveKitSession session_;
    Esp32HubConfig config_;
    std::string config_url_;
    VoiceSessionState state_ = VoiceSessionState::Idle;
    bool mic_enabled_ = true;
    StateCallback on_state_changed_;
};

}  // namespace eidolon

#endif  // EIDOLON_VOICE_CONTROLLER_H_
