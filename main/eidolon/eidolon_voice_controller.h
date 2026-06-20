#ifndef EIDOLON_VOICE_CONTROLLER_H_
#define EIDOLON_VOICE_CONTROLLER_H_

#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>

#include "hub_types.h"
#include "livekit_session.h"

namespace eidolon {

struct ControlCommand;

enum class VoiceSessionState {
    Idle,
    PendingApproval,
    WaitingBinding,
    ConfigReady,
    Connecting,
    InRoom,
    Reconnecting,
    Error,
    Unauthorized,       // revoked or unregistered by admin
    ServerUnreachable,  // repeated connect failures; re-discovery exhausted
};

class EidolonVoiceController {
public:
    using StateCallback = std::function<void(VoiceSessionState)>;

    void OnHubActivationSucceeded();
    void OnNetworkLost();

    esp_err_t JoinRoom();
    esp_err_t LeaveRoom();
    esp_err_t SetMicEnabled(bool enabled);

    // Push-to-talk (hold-to-talk). Press opens the mic (joining the room first if
    // needed) and marks the turn as in progress; release closes the mic and the
    // ptt=false edge tells the server the turn is complete. No-op when PTT mode
    // is disabled (CONFIG_EIDOLON_INTERACTION_MODE_PTT off → full-duplex/auto).
    void OnPttPressed();
    void OnPttReleased();
    bool IsPttMode() const { return ptt_mode_; }
    bool IsPttHeld() const { return ptt_active_; }

    VoiceSessionState GetState() const { return state_; }
    void SetOnStateChanged(StateCallback cb) { on_state_changed_ = std::move(cb); }
    void SetOnTranscription(std::function<void(const TranscriptionEvent&)> cb);
    void SetOnAgentPhase(std::function<void(AgentPhase)> cb);

private:
    esp_err_t LoadStoredConfig();
    esp_err_t RefreshHubConfig();
    // Re-query mDNS for the Hub, and if its address changed, adopt the new
    // config_url and re-fetch config. Recovers from a Hub IP change (DHCP /
    // network move) that left the cached address dead.
    esp_err_t RediscoverHub();
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
    // Acks `command` as accepted, then runs `handler` on a short-lived task.
    // Replies "failed/TASK_CREATE_FAILED" if the task cannot be created.
    void SpawnCommandTask(const char* task_name, const ControlCommand& command,
                          void (EidolonVoiceController::*handler)(const std::string&));
    void ScheduleControlReconnect(const char* reason);
    // Connect watchdog: a connect/reconnect attempt that never reaches a terminal
    // LiveKit state (Connected/Failed/Disconnected) would otherwise leave the
    // session stuck in Connecting/Reconnecting forever. Armed whenever the state
    // is (Re)connecting, disarmed on any other state; on timeout it tears down the
    // hung attempt and reschedules a reconnect. Mode-agnostic (PTT and full-duplex
    // share this connection path).
    void ArmConnectWatchdog();
    void DisarmConnectWatchdog();
    void HandleConnectTimeout();
    static void ConnectWatchdogCb(void* arg);
    // Idle auto-leave (PTT only): after a stretch in-room with no PTT activity and
    // no agent output, leave the voice room (back to the control room / ready
    // state) so the server can release the agent session. Re-armed on any activity;
    // re-entering the room is an explicit user action (tap to connect). Full-duplex
    // keeps the server-side idle policy instead.
    void UpdateIdleAutoLeave();
    void HandleIdleAutoLeave();
    static void IdleLeaveCb(void* arg);
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
    // Interaction mode: push-to-talk (half-duplex) vs auto open-mic (full-duplex).
    // Compile-time per board via Kconfig; runtime field keeps the branch readable.
    bool ptt_mode_ =
#if CONFIG_EIDOLON_INTERACTION_MODE_PTT
        true;
#else
        false;
#endif
    bool ptt_active_ = false;  // PTT: button currently held (mic open this turn)
    bool control_room_ = false;
    bool switching_to_voice_ = false;
    bool control_reconnect_pending_ = false;
    // Consecutive reconnect attempts since the last successful connect. Drives
    // backoff, when to re-discover the Hub, and the ServerUnreachable UI.
    int reconnect_attempts_ = 0;
    esp_timer_handle_t connect_watchdog_ = nullptr;
    esp_timer_handle_t idle_leave_timer_ = nullptr;
    volatile bool audio_state_task_stop_ = false;
    bool audio_state_task_running_ = false;
    uint32_t audio_state_seq_ = 0;
    bool audio_state_sent_ = false;
    bool last_audio_playback_active_ = false;
    bool last_audio_mic_muted_ = false;
    bool last_audio_ptt_ = false;
    int64_t last_audio_publish_us_ = 0;
    AgentPhase agent_phase_ = AgentPhase::Silent;
    StateCallback on_state_changed_;
    std::function<void(AgentPhase)> on_agent_phase_;
};

}  // namespace eidolon

#endif  // EIDOLON_VOICE_CONTROLLER_H_
