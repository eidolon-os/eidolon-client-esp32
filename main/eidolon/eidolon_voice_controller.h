#ifndef EIDOLON_VOICE_CONTROLLER_H_
#define EIDOLON_VOICE_CONTROLLER_H_

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>

#include "hub_types.h"
#include "livekit_session.h"

namespace eidolon {

enum class VoiceSessionState {
    Idle,
    PendingApproval,
    WaitingBinding,
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
    void SetOnTranscription(std::function<void(const TranscriptionEvent&)> cb);
    void SetOnAgentPhase(std::function<void(AgentPhase)> cb);

private:
    esp_err_t LoadStoredConfig();
    esp_err_t RefreshHubConfig();
    esp_err_t ConnectControlRoom();
    bool HasActiveConfig() const;
    bool HasControlConfig() const;
    VoiceSessionState StateForConfig(const Esp32HubConfig& config) const;
    void SetState(VoiceSessionState state);
    void OnLiveKitState(LiveKitConnectionState lk_state);
    void OnControlCommand(const std::string& payload);
    void OnSessionControl(const std::string& payload);
    void HandleConfigRefreshCommand(const std::string& command_id);
    void HandleRoomJoinCommand(const std::string& command_id);
    void HandlePlaybackStopCommand(const std::string& command_id);
    void HandleIdleTimeoutCommand();
    void ScheduleControlReconnect(const char* reason);
    void StartAudioStatePublisher();
    void StopAudioStatePublisher();
    void AudioStatePublisherTask();
    void PublishClientAudioState(bool playback_active);
    bool PlaybackActiveRecently() const;
    bool AgentOutputActiveRecently() const;
    void HandleAgentPhase(AgentPhase phase);

    LiveKitSession session_;
    Esp32HubConfig config_;
    std::string config_url_;
    VoiceSessionState state_ = VoiceSessionState::Idle;
    bool mic_enabled_ = true;
    bool control_room_ = false;
    bool switching_to_voice_ = false;
    bool control_reconnect_pending_ = false;
    volatile bool audio_state_task_stop_ = false;
    bool audio_state_task_running_ = false;
    uint32_t audio_state_seq_ = 0;
    bool audio_state_sent_ = false;
    bool last_audio_playback_active_ = false;
    bool last_audio_mic_muted_ = false;
    AgentPhase agent_phase_ = AgentPhase::Silent;
    StateCallback on_state_changed_;
    std::function<void(AgentPhase)> on_agent_phase_;
};

}  // namespace eidolon

#endif  // EIDOLON_VOICE_CONTROLLER_H_
