#ifndef EIDOLON_VOICE_CONTROLLER_H_
#define EIDOLON_VOICE_CONTROLLER_H_

#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <deque>
#include <functional>
#include <string>
#include <type_traits>

#include "sdkconfig.h"
#include "ambient_presence_state.h"
#include "control_protocol.h"
#include "channel_recovery.h"
#include "device_event_bus.h"
#include "eidolon_device_profile.h"
#if CONFIG_EIDOLON_GUARD_SERVICE
#include "guard/guard_presence_adapter.h"
#include "guard/guard_service.h"
#include "guard/owner_presence_adapter.h"
#endif
#include "hub_types.h"
#include "livekit_session.h"

namespace eidolon {

class GuardService;

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

// Single-task actor. Every external entry point (PTT, join/leave, mic, network,
// activation) and every LiveKit/SDK callback only *posts an event*; all state is
// mutated exclusively on the one controller task that drains the event queue, so
// there are no data races and no scattered per-command/reconnect/audio tasks.
// Mode-agnostic: half-duplex (PTT) and full-duplex (barge-in) share this loop and
// differ only inside a few handlers (mic gating, PTT events, idle fallback).
class EidolonVoiceController {
public:
    using StateCallback = std::function<void(VoiceSessionState)>;

    explicit EidolonVoiceController(GuardService* guard_service = nullptr);
    ~EidolonVoiceController();

    void OnHubActivationSucceeded();
    void OnNetworkLost();
    void OnNetworkRestored();
    void OnAmbientPresenceChanged(bool present);

    esp_err_t JoinRoom();
    esp_err_t LeaveRoom();
    esp_err_t SetMicEnabled(bool enabled);

    // Push-to-talk (hold-to-talk). Press opens the mic; release keeps a short
    // capture tail, then closes it and sends the ptt=false edge that tells the
    // server the turn is complete. No-op when PTT mode is disabled.
    void OnPttPressed();
    void OnPttReleased();
    bool IsPttMode() const { return ptt_mode_; }
    bool IsPttHeld() const { return ptt_active_; }
    // Half-duplex: auto open-mic that closes while the agent speaks (no device
    // AEC). Full-duplex = neither PTT nor half-duplex (open mic + AEC + barge-in).
    bool IsHalfDuplexMode() const { return half_duplex_mode_; }
    bool IsFullDuplex() const { return !ptt_mode_ && !half_duplex_mode_; }

    // Read directly off the controller-owned field. state_ is a word-sized enum
    // written only on the controller task; a cross-thread read is benign (returns a
    // recent value) and the authoritative decisions re-check state on the task.
    VoiceSessionState GetState() const { return state_; }
    static const char* VoiceStateName(VoiceSessionState state);
    // Why the last voice session ended (None until the channel reports one via
    // session_end). Read by the UI to distinguish a normal end from a JOIN
    // failure. Word-sized enum written only on the controller task; a cross-thread
    // read is benign (recent value).
    EndReason LastEndReason() const { return last_end_reason_; }
    void SetOnStateChanged(StateCallback cb) { on_state_changed_ = std::move(cb); }
    void SetOnTranscription(std::function<void(const TranscriptionEvent&)> cb);
    void SetOnAgentPhase(std::function<void(AgentPhase)> cb);
    void SetOnPresenceWakePhase(std::function<void(PresenceWakePhase)> cb)
    {
        on_presence_wake_phase_ = std::move(cb);
    }
    void SetOnPttTurnStatus(std::function<void(const std::string&)> cb)
    {
        on_ptt_turn_status_ = std::move(cb);
    }
    bool RegisterDeviceEventHandler(const std::string& type, DeviceEventBus::Handler handler);
    esp_err_t PublishDeviceEvent(const std::string& payload);

private:
    // ---- Event loop ----
    enum class EventType {
        Activation,
        NetworkLost,
        NetworkRestored,
        Join,
        Leave,
        SetMic,
        PttPress,
        PttRelease,
        PttReleaseTail,
        LiveKitState,
#if CONFIG_EIDOLON_GUARD_SERVICE
        GuardObservation,
        OwnerPresence,
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
        OwnerFaceProfileCompleted,
#endif
        ControlCommand,
        SessionControl,
        DeviceEvent,
        PublishDeviceEvent,
        AmbientPresence,
        AmbientPresenceTimer,
        AgentPhaseChanged,
        SessionActivity,
        AudioTick,
        OnboardingPoll,
        ReconnectTick,
        ConnectTimeout,
        IdleLeave,
        FullDuplexIdleFallback,
    };
    struct Event {
        EventType type;
        LiveKitConnectionState lk_state = LiveKitConnectionState::Disconnected;
        AgentPhase phase = AgentPhase::Silent;
        bool flag = false;
#if CONFIG_EIDOLON_GUARD_SERVICE
        GuardObservation guard_observation;
        OwnerPresenceObservation owner_presence_observation;
#endif
        // Snapshot of session_generation_ taken when the SDK callback fired (on the
        // SDK task), so DoLiveKitState can tell whether a LiveKit event belongs to
        // the connection attempt that is still current or to a superseded one whose
        // late teardown event would otherwise be misattributed (Phase 0: logged;
        // Phase 1: dropped on mismatch).
        uint32_t generation = 0;
        std::string* payload = nullptr;  // owned; the loop deletes it after dispatch
    };
    static_assert(std::is_trivially_copyable_v<Event>,
                  "FreeRTOS queue events must be safe for raw byte copies");
    static void TaskTrampoline(void* arg);
    void ControllerLoop();
    void Enqueue(Event ev);  // thread-safe; frees owned pointers if the queue is full
    void Dispatch(const Event& ev);

    // ---- Handlers (run only on the controller task) ----
    void DoActivation();
    void DoNetworkLost();
    void DoNetworkRestored();
    esp_err_t DoJoinRoom();
    esp_err_t DoLeaveRoom();
    void DoSetMicEnabled(bool enabled);
    void DoPttPressed();
    void DoPttReleased();
    void DoPttReleaseTail();
    void DoLiveKitState(LiveKitConnectionState lk_state, uint32_t event_generation);
#if CONFIG_EIDOLON_GUARD_SERVICE
    void DoGuardObservation(const GuardObservation& observation, uint32_t runtime_generation);
    void DoOwnerPresence(const OwnerPresenceObservation& observation,
                         uint32_t runtime_generation);
    void HandleAmbientPresenceEvent(const DeviceEventMessage& event);
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    void DoOwnerFaceProfileCompleted(const std::string& payload);
#endif
    void DoControlCommand(const std::string& payload);
    void DoSessionControl(const std::string& payload);
    void DoDeviceEvent(const std::string& payload, uint32_t event_generation);
    void DoPublishDeviceEvent(const std::string& payload);
    void DoAmbientPresenceChanged(bool present);
    void DoAmbientPresenceTimer();
    void PublishRadarPresenceState(AmbientPresenceObservation observation);
    void OpenConversationAudio();
    void CloseConversationAudio();
    void ScheduleAmbientPresenceTimer(uint64_t delay_ms);
#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    void ScheduleAmbientPresenceLeaseExpiry(uint64_t now_ms);
    void PublishPendingOwnerConfirmations(
        const OwnerPresenceObservation& owner_presence);
    bool PublishOwnerConfirmation(
        const AmbientPresenceAssertion& assertion,
        const OwnerPresenceObservation& owner_presence);
#endif
    void DoAgentPhase(AgentPhase phase);
    void DoSessionActivity();
    void DoAudioTick();
    void DoOnboardingPoll();
    void DoReconnectTick();
    void DoConnectTimeout();
    void DoIdleAutoLeave();
    void DoFullDuplexIdleFallback();

    // ---- Internal helpers (controller task only) ----
    esp_err_t LoadStoredConfig();
    esp_err_t LoadAuthorityRoutes();
    // Fetch fresh Hub config (server_url/token/room_name/...). persist=true also
    // writes it to NVS; the per-JOIN refresh passes persist=false because each
    // JOIN now gets a unique nonce'd voice room+token (so NVS dedup would never
    // hit and every JOIN would needlessly wear flash).
    esp_err_t RefreshHubConfig(bool persist = true);
    esp_err_t RediscoverHub();
    esp_err_t ConnectChannel();
    esp_err_t PublishSessionRequest(const char* type);
    bool HasActiveConfig() const;
    bool HasChannelConfig() const;
    VoiceSessionState StateForConfig(const Esp32HubConfig& config) const;
    void SetState(VoiceSessionState state, const char* reason = "unspecified");
    // Begin a new connection attempt: bump session_generation_, remember which
    // plane (control/voice) it is for, and log the transition. Every event from a
    // prior generation is, by definition, stale. Returns the new generation.
    uint32_t BeginSessionGeneration(const char* room_kind);
    // Abandon the current connection without immediately starting a new one: bump
    // the generation so any in-flight events from the connection we are tearing
    // down (a hung attempt, a network drop) are dropped by the generation gate
    // rather than accepted after we have moved on. Used by teardown paths that do
    // not synchronously call Begin*Generation themselves.
    void MarkSessionSuperseded(const char* reason);
    // Plane that the live session currently belongs to ("control"/"voice"/"none"),
    // for diagnostic log attribution.
    const char* CurrentRoomKind() const;
    // Control-op handlers. Each receives the command id (for the ACK) and the
    // raw JSON ``payload`` string (op-specific args). room.join reads the
    // session_intent out of the payload; the others ignore it.
    void HandleConfigRefreshCommand(const std::string& command_id, const std::string& payload);
    void HandleRoomJoinCommand(const std::string& command_id, const std::string& payload);
    void HandlePlaybackStopCommand(const std::string& command_id, const std::string& payload);
    void HandlePttTurnStatusCommand(const std::string& command_id, const std::string& payload);
    void HandleDeviceIdentifyCommand(const std::string& command_id, const std::string& payload);
    void HandleHeadLookAtCommand(const std::string& command_id, const std::string& payload);
    void HandleHeadHomeCommand(const std::string& command_id, const std::string& payload);
    void HandleHeadGestureCommand(const std::string& command_id, const std::string& payload);
    void HandleSafetyStopCommand(const std::string& command_id, const std::string& payload);
    void HandlePresenceSetCommand(const std::string& command_id, const std::string& payload);
#if CONFIG_EIDOLON_GUARD_SERVICE
    void HandleDeviceRollCallCommand(const std::string& command_id, const std::string& payload);
#endif
#if CONFIG_EIDOLON_GUARD_SERVICE
    void HandleGuardRuntimeSyncCommand(const std::string& command_id, const std::string& payload);
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    void HandleGuardOwnerFaceProfileSyncCommand(const std::string& command_id,
                                                const std::string& payload);
#endif
#if CONFIG_EIDOLON_GUARD_VISION_BENCHMARK
    void HandleGuardVisionBenchmarkCommand(const std::string& command_id, const std::string& payload);
#endif
    void HandleIdleTimeoutCommand();
    void HandleOwnerPresenceConfirmedEvent(const DeviceEventMessage& event);
    void HandleOwnerPresenceChangedEvent(const DeviceEventMessage& event);
    void PublishFlowNode(const std::string& flow_id,
                         const std::string& causation_id,
                         const char* stage, const char* status,
                         const char* label);
    void ResetPresenceManagedSession();
    void CheckOwnerPresenceLease();
    // Parse and act on a session_end{reason} packet from the channel: record the
    // reason for the UI, tear the voice room down gracefully, and pick the
    // resulting state (Ready for a normal end, Error for a server error).
    void HandleSessionEnd(EndReason reason);
    static EndReason ParseEndReason(const std::string& payload);
    esp_err_t AckCommand(const ControlCommand& command, const char* status, const char* code,
                         const char* detail = "", const char* result = "");
#if CONFIG_EIDOLON_GUARD_SERVICE
    esp_err_t SyncGuardRuntime(const char* reason, const std::string* expected_binding_id = nullptr,
                               uint32_t expected_runtime_revision = 0,
                               const std::string* expected_desired_state = nullptr,
                               uint32_t* applied_runtime_revision = nullptr);
    void ConfigureGuardPresenceRuntime(const GuardRuntimeHubConfig& runtime);
    void ClearGuardPresenceRuntime();
    void FlushPendingGuardPresence();
    static uint64_t GuardEventTimestampMs(uint64_t monotonic_ms);
#endif
    void CompletePendingRoomJoinCommand(const char* status, const char* code,
                                        const char* detail = "",
                                        const char* result = "");
    const char* JoinBlockedCode() const;

    // Reconnect (timer-driven backoff) + connect watchdog + idle fallbacks. Each
    // timer callback just posts an event; the work runs on the controller task.
    void ScheduleChannelReconnect(const char* reason);
    void ScheduleOnboardingPoll();
    void ArmConnectWatchdog();
    void DisarmConnectWatchdog();
    void UpdateIdleAutoLeave();
    void ResetFullDuplexIdleFallback(const char* reason);
    void DisarmFullDuplexIdleFallback();
    void CancelPttReleaseTail();
    void FinalizePttRelease(const char* reason);
    static void ReconnectTimerCb(void* arg);
    static void OnboardingPollTimerCb(void* arg);
    static void ConnectWatchdogCb(void* arg);
    static void IdleLeaveCb(void* arg);
    static void FullDuplexIdleFallbackCb(void* arg);
    static void PttReleaseTailCb(void* arg);
    static void AmbientPresenceTimerCb(void* arg);
    void SetPresenceWakePhase(PresenceWakePhase phase);

    // Audio-state publisher (timer-driven tick on the controller task).
    void StartAudioStatePublisher();
    void StopAudioStatePublisher();
    static void AudioTimerCb(void* arg);
    void PublishClientAudioState(bool playback_active);
    bool PlaybackActiveRecently() const;
    bool AgentOutputActiveRecently() const;
    void UpdateLocalPlaybackPhase(bool playback_active);
    esp_err_t StopLocalPlayback(const char* reason);

    LiveKitSession session_;
    DeviceEventBus device_event_bus_;
    Esp32HubConfig config_;
    std::string device_control_uri_;
    VoiceSessionState state_ = VoiceSessionState::Idle;
    bool mic_enabled_ = true;
    // Interaction mode (one of three, compile-time per board via Kconfig):
    //   ptt_mode_        -> push-to-talk (mic open only while the button is held)
    //   half_duplex_mode_-> auto open-mic, closed while the agent speaks (no AEC)
    //   neither          -> full-duplex (open mic + device AEC + barge-in)
    // Runtime fields keep the capture-gate branch readable; the Kconfig -> mode
    // mapping itself lives once in eidolon_device_profile.h, which also rejects
    // full_duplex on a board without a validated AEC reference at compile time.
    bool ptt_mode_ = kModePtt;
    bool half_duplex_mode_ = kModeHalfDuplex;
    bool ptt_active_ = false;  // PTT: held or in the short release tail (mic open)
    bool ptt_release_tail_pending_ = false;
    // Session intent for the NEXT voice JOIN, set by an orchestrated room.join
    // or verified owner-presence wake and consumed by DoJoinRoom so it rides
    // the token fetch as X-Device-Session-Intent. Empty for a normal user JOIN.
    // Controller-task only.
    std::string pending_session_intent_;
    std::string pending_session_flow_id_;
    std::string current_presence_flow_id_;
    std::string owner_lease_source_device_id_;
    uint64_t owner_lease_deadline_ms_ = 0;
    uint32_t owner_lease_guard_epoch_ = 0;
    uint32_t owner_lease_sequence_ = 0;
    bool presence_managed_voice_session_ = false;
    // Connected to the channel, but not in a conversation. It intentionally
    // covers Connecting/Reconnecting/Connected; actual health is tracked by
    // channel_recovery_ / LiveKitSession::IsConnected().
    //
    // This used to be "I am in the control room rather than the voice room" —
    // the same distinction, when it was still drawn by which room the device
    // stood in. The channel no longer moves, so the device says which of the
    // two it is.
    bool standby_ = false;
    GuardService* guard_service_ = nullptr;
#if CONFIG_EIDOLON_GUARD_SERVICE
    RoomConfig guard_control_config_;
    bool has_guard_control_config_ = false;
    GuardPresenceAdapter guard_presence_adapter_;
    OwnerPresenceAdapter owner_presence_adapter_;
    std::deque<std::string> pending_guard_presence_payloads_;
    uint32_t guard_runtime_generation_ = 0;
#endif
    bool switching_to_voice_ = false;
    ChannelRecovery channel_recovery_;
    bool pending_room_join_command_active_ = false;
    uint32_t pending_room_join_generation_ = 0;
    ControlCommand pending_room_join_command_;
    // Monotonic attempt id, bumped at the start of every voice/control connect.
    // The state-changed callback snapshots it into the event so a late teardown
    // from a superseded connection can be recognised (Phase 0 logs the mismatch;
    // Phase 1 drops it). Also the seed for the proactive wake "generation" the
    // plan reserves for Phase 3.
    uint32_t session_generation_ = 0;
    // Why the last voice session ended; surfaced to the UI. Cleared (→ None) when
    // a new voice room is requested/connected so an old reason never bleeds into
    // a fresh session's chrome.
    EndReason last_end_reason_ = EndReason::None;
    bool audio_publisher_active_ = false;
    uint32_t audio_state_seq_ = 0;
    bool audio_state_sent_ = false;
    bool last_audio_playback_active_ = false;
    bool local_playback_ui_active_ = false;
    bool last_audio_mic_muted_ = false;
    bool last_audio_ptt_ = false;
    int64_t last_audio_publish_us_ = 0;
    AgentPhase agent_phase_ = AgentPhase::Silent;

    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    esp_timer_handle_t audio_timer_ = nullptr;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    esp_timer_handle_t onboarding_poll_timer_ = nullptr;
    esp_timer_handle_t connect_watchdog_ = nullptr;
    esp_timer_handle_t idle_leave_timer_ = nullptr;
    esp_timer_handle_t full_duplex_idle_timer_ = nullptr;
    esp_timer_handle_t ptt_release_tail_timer_ = nullptr;
    esp_timer_handle_t ambient_presence_timer_ = nullptr;

    StateCallback on_state_changed_;
    std::function<void(const TranscriptionEvent&)> on_transcription_;
    std::function<void(AgentPhase)> on_agent_phase_;
    std::function<void(PresenceWakePhase)> on_presence_wake_phase_;
    std::function<void(const std::string&)> on_ptt_turn_status_;
#if CONFIG_EIDOLON_RADAR_PRESENCE_BROADCAST
    bool radar_presence_known_ = false;
    bool radar_present_ = false;
    bool radar_presence_dirty_ = false;
    uint32_t radar_presence_epoch_ = 0;
    uint32_t radar_presence_sequence_ = 0;
    std::string radar_presence_flow_id_;
    AmbientPresenceObservation radar_pending_observation_ =
        AmbientPresenceObservation::Snapshot;
    AmbientPresenceActivationGate radar_activation_gate_;
#endif
#if CONFIG_EIDOLON_AMBIENT_PRESENCE_OWNER_AUTH
    AmbientPresenceRegistry ambient_presence_registry_;
#endif
    PresenceWakePhase presence_wake_phase_ = PresenceWakePhase::Idle;
};

}  // namespace eidolon

#endif  // EIDOLON_VOICE_CONTROLLER_H_
