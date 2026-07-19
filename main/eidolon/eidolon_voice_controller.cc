#include "eidolon_voice_controller.h"

#include "board.h"
#include "control_protocol.h"
#include "eidolon_topics.h"
#include "eidolon_local_feedback.h"
#if CONFIG_EIDOLON_GUARD_SERVICE
#include "guard/guard_service.h"
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
#include "guard/owner_face_engine.h"
#endif
#if CONFIG_EIDOLON_GUARD_VISION_BENCHMARK
#include "guard/vision_benchmark.h"
#endif
#include "hub_config_client.h"
#include "hub_config_store.h"
#include "hub_discovery.h"
#include "livekit_board.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstring>
#include <ctime>
#include <new>
#include <stdio.h>

#ifndef CONFIG_EIDOLON_PTT_RELEASE_TAIL_MS
#define CONFIG_EIDOLON_PTT_RELEASE_TAIL_MS 0
#endif

#ifndef CONFIG_EIDOLON_PTT_IDLE_FALLBACK_MS
#define CONFIG_EIDOLON_PTT_IDLE_FALLBACK_MS 0
#endif

#ifndef CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS
#define CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS 0
#endif

#define TAG "EidolonVoice"

namespace {
constexpr int64_t kPlaybackActiveWindowUs = 1200 * 1000;
// Poll the audio state fast so a barge-in (near-end speech) edge reaches the
// channel within ~one poll, but only emit an unchanged heartbeat every
// kAudioStateHeartbeatUs to avoid flooding lossy packets at the poll rate.
constexpr uint64_t kAudioTickIntervalUs = 80 * 1000;
constexpr int64_t kAudioStateHeartbeatUs = 500 * 1000;
// A connect/reconnect attempt that never reaches a terminal LiveKit state within
// this long is treated as hung and force-recovered. Generous enough to cover a
// slow mDNS + HTTPS + connect on a healthy-but-slow network.
constexpr uint64_t kConnectWatchdogUs = 25ULL * 1000 * 1000;
// PTT: Channel owns half-duplex idle teardown and sends session_end before
// deleting the voice room. This client timer is a lifecycle fallback only, so a
// missed data packet / room-close callback cannot leave the UI stuck in a voice
// room. Default 0 disables it.
constexpr uint64_t kPttIdleFallbackUs =
    static_cast<uint64_t>(CONFIG_EIDOLON_PTT_IDLE_FALLBACK_MS) * 1000ULL;
// PTT release tail: keep capture open briefly after touch release before
// publishing ptt=false. This avoids clipping the final phoneme while preserving
// an explicit, device-owned turn boundary.
constexpr uint64_t kPttReleaseTailUs =
    static_cast<uint64_t>(CONFIG_EIDOLON_PTT_RELEASE_TAIL_MS) * 1000ULL;
// Full-duplex: Channel owns idle teardown and sends session_end before deleting
// the voice room. This client timer is a lifecycle fallback only, so a missed
// data packet / room-close callback cannot leave the UI stuck in "listening".
constexpr uint64_t kFullDuplexIdleFallbackUs =
    static_cast<uint64_t>(CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS) * 1000ULL;
// Single controller task: drains the event queue, serializing all state mutation.
// Stack sized for the heaviest handler (rediscover = mDNS + HTTPS config fetch +
// mbedtls signing + connect), which the old reconnect task ran on 8192.
constexpr int kControllerTaskStack = 8192;
constexpr UBaseType_t kControllerTaskPriority = 5;
constexpr UBaseType_t kEventQueueLen = 24;
// Let the control-room ack flush before switching to the voice room.
constexpr TickType_t kRoomJoinSettleDelay = pdMS_TO_TICKS(500);
// Let the "succeeded" ack flush before reconnecting the control room.
constexpr TickType_t kActiveAckSettleDelay = pdMS_TO_TICKS(100);
#if CONFIG_EIDOLON_GUARD_SERVICE
constexpr size_t kMaxPendingGuardPresenceEvents = 8;
#endif

const char* LiveKitConnectionStateName(eidolon::LiveKitConnectionState state)
{
    switch (state) {
    case eidolon::LiveKitConnectionState::Disconnected:
        return "Disconnected";
    case eidolon::LiveKitConnectionState::Connecting:
        return "Connecting";
    case eidolon::LiveKitConnectionState::Connected:
        return "Connected";
    case eidolon::LiveKitConnectionState::Reconnecting:
        return "Reconnecting";
    case eidolon::LiveKitConnectionState::Failed:
        return "Failed";
    }
    return "unknown";
}

const char* AgentPhaseName(eidolon::AgentPhase phase)
{
    switch (phase) {
    case eidolon::AgentPhase::Silent:
        return "silent";
    case eidolon::AgentPhase::UserSpeaking:
        return "user_speaking";
    case eidolon::AgentPhase::AgentThinking:
        return "agent_thinking";
    case eidolon::AgentPhase::AgentSpeaking:
        return "agent_speaking";
    }
    return "unknown";
}
}  // namespace

namespace eidolon {

// ============================ Event loop plumbing ============================

EidolonVoiceController::EidolonVoiceController(GuardService* guard_service)
    : guard_service_(guard_service)
{
    event_queue_ = xQueueCreate(kEventQueueLen, sizeof(Event));
    if (event_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create controller event queue");
        return;
    }
    if (xTaskCreate(&EidolonVoiceController::TaskTrampoline, "eidolon_ctrl",
                    kControllerTaskStack, this, kControllerTaskPriority, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create controller task");
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
}

EidolonVoiceController::~EidolonVoiceController()
{
    // Best-effort teardown; in practice the controller lives for the app lifetime.
    for (esp_timer_handle_t* t :
         {&audio_timer_, &reconnect_timer_, &connect_watchdog_, &idle_leave_timer_,
          &full_duplex_idle_timer_, &ptt_release_tail_timer_}) {
        if (*t != nullptr) {
            esp_timer_stop(*t);
            esp_timer_delete(*t);
            *t = nullptr;
        }
    }
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (event_queue_ != nullptr) {
        Event ev;
        while (xQueueReceive(event_queue_, &ev, 0) == pdTRUE) {
            delete ev.payload;
        }
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
}

void EidolonVoiceController::TaskTrampoline(void* arg)
{
    static_cast<EidolonVoiceController*>(arg)->ControllerLoop();
    vTaskDelete(nullptr);
}

void EidolonVoiceController::ControllerLoop()
{
    Event ev;
    for (;;) {
        if (xQueueReceive(event_queue_, &ev, portMAX_DELAY) == pdTRUE) {
            Dispatch(ev);
            delete ev.payload;  // null-safe; only set for string-carrying events
        }
    }
}

void EidolonVoiceController::Enqueue(Event ev)
{
    if (event_queue_ == nullptr) {
        delete ev.payload;
        return;
    }
    // The queue copies the struct (including the payload pointer); on success the
    // loop owns and frees it, on failure we free it here.
    if (xQueueSend(event_queue_, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full; dropped event type=%d", static_cast<int>(ev.type));
        delete ev.payload;
    }
}

void EidolonVoiceController::Dispatch(const Event& ev)
{
    switch (ev.type) {
    case EventType::Activation:
        DoActivation();
        break;
    case EventType::NetworkLost:
        DoNetworkLost();
        break;
    case EventType::NetworkRestored:
        DoNetworkRestored();
        break;
    case EventType::Join:
        DoJoinRoom();
        break;
    case EventType::Leave:
        DoLeaveRoom();
        break;
    case EventType::SetMic:
        DoSetMicEnabled(ev.flag);
        break;
    case EventType::PttPress:
        DoPttPressed();
        break;
    case EventType::PttRelease:
        DoPttReleased();
        break;
    case EventType::PttReleaseTail:
        DoPttReleaseTail();
        break;
    case EventType::LiveKitState:
        DoLiveKitState(ev.lk_state, ev.generation);
        break;
#if CONFIG_EIDOLON_GUARD_SERVICE
    case EventType::GuardObservation:
        DoGuardObservation(ev.guard_observation, ev.generation);
        break;
    case EventType::OwnerPresence:
        DoOwnerPresence(ev.owner_presence_observation, ev.generation);
        break;
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
    case EventType::OwnerFaceProfileCompleted:
        if (ev.payload != nullptr) {
            DoOwnerFaceProfileCompleted(*ev.payload);
        }
        break;
#endif
    case EventType::ControlCommand:
        if (ev.payload != nullptr) {
            DoControlCommand(*ev.payload);
        }
        break;
    case EventType::SessionControl:
        if (ev.payload != nullptr) {
            DoSessionControl(*ev.payload);
        }
        break;
    case EventType::AgentPhaseChanged:
        DoAgentPhase(ev.phase);
        break;
    case EventType::SessionActivity:
        DoSessionActivity();
        break;
    case EventType::AudioTick:
        DoAudioTick();
        break;
    case EventType::ReconnectTick:
        DoReconnectTick();
        break;
    case EventType::ConnectTimeout:
        DoConnectTimeout();
        break;
    case EventType::IdleLeave:
        DoIdleAutoLeave();
        break;
    case EventType::FullDuplexIdleFallback:
        DoFullDuplexIdleFallback();
        break;
    }
}

// ============================ Public entry points ============================
// All just post an event; the work happens on the controller task. Returns are
// ignored by callers (LiveKitVoiceTransport), so success/failure is reported via
// state transitions, not the synchronous return.

void EidolonVoiceController::OnHubActivationSucceeded()
{
    Event ev;
    ev.type = EventType::Activation;
    Enqueue(ev);
}

void EidolonVoiceController::OnNetworkLost()
{
    ESP_LOGW(TAG, "[lifecycle] enqueue network_lost state=%s room_kind=%s gen=%lu",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    Event ev;
    ev.type = EventType::NetworkLost;
    Enqueue(ev);
}

void EidolonVoiceController::OnNetworkRestored()
{
    ESP_LOGI(TAG, "[lifecycle] enqueue network_restored state=%s room_kind=%s gen=%lu",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    Event ev;
    ev.type = EventType::NetworkRestored;
    Enqueue(ev);
}

esp_err_t EidolonVoiceController::JoinRoom()
{
    ESP_LOGI(TAG, "[lifecycle] enqueue join state=%s room_kind=%s gen=%lu",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    Event ev;
    ev.type = EventType::Join;
    Enqueue(ev);
    return ESP_OK;
}

esp_err_t EidolonVoiceController::LeaveRoom()
{
    ESP_LOGI(TAG, "[lifecycle] enqueue leave state=%s room_kind=%s gen=%lu",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    Event ev;
    ev.type = EventType::Leave;
    Enqueue(ev);
    return ESP_OK;
}

esp_err_t EidolonVoiceController::SetMicEnabled(bool enabled)
{
    Event ev;
    ev.type = EventType::SetMic;
    ev.flag = enabled;
    Enqueue(ev);
    return ESP_OK;
}

void EidolonVoiceController::OnPttPressed()
{
    ESP_LOGI(TAG, "[ptt] enqueue press state=%s room_kind=%s gen=%lu ptt_active=%d tail=%d",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0);
    Event ev;
    ev.type = EventType::PttPress;
    Enqueue(ev);
}

void EidolonVoiceController::OnPttReleased()
{
    ESP_LOGI(TAG, "[ptt] enqueue release state=%s room_kind=%s gen=%lu ptt_active=%d tail=%d",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0);
    Event ev;
    ev.type = EventType::PttRelease;
    Enqueue(ev);
}

void EidolonVoiceController::SetOnTranscription(std::function<void(const TranscriptionEvent&)> cb)
{
    on_transcription_ = std::move(cb);
    session_.SetOnTranscription([this](const TranscriptionEvent& event) {
        if (!event.text.empty()) {
            Event ev;
            ev.type = EventType::SessionActivity;
            Enqueue(ev);
        }
        if (on_transcription_) {
            on_transcription_(event);
        }
    });
}

void EidolonVoiceController::SetOnAgentPhase(std::function<void(AgentPhase)> cb)
{
    on_agent_phase_ = std::move(cb);
    session_.SetOnAgentPhase([this](AgentPhase phase) {
        Event ev;
        ev.type = EventType::AgentPhaseChanged;
        ev.phase = phase;
        Enqueue(ev);
    });
}

// ============================ Config / state helpers ============================

VoiceSessionState EidolonVoiceController::StateForConfig(const Esp32HubConfig& config) const
{
    switch (config.status) {
    case HubConfigStatus::PendingApproval:
        return VoiceSessionState::PendingApproval;
    case HubConfigStatus::WaitingBinding:
        return VoiceSessionState::WaitingBinding;
    case HubConfigStatus::Active:
        return VoiceSessionState::ConfigReady;
    case HubConfigStatus::Revoked:
    case HubConfigStatus::Unregistered:
        return VoiceSessionState::Unauthorized;
    }
    return VoiceSessionState::Error;
}

bool EidolonVoiceController::HasActiveConfig() const
{
#if CONFIG_EIDOLON_GUARD_SERVICE
    // Guard registration receives a stable data-only control room in the
    // legacy `active` slot.  It must never be treated as a normal voice room.
    return false;
#else
    return config_.status == HubConfigStatus::Active && config_.active.usable();
#endif
}

bool EidolonVoiceController::HasControlConfig() const
{
    if (config_.status != HubConfigStatus::Active) {
        // While pending/waiting, the "active" slot holds the pending room used
        // as the data-only control channel.
        return config_.active.usable();
    }
    return config_.control.usable() && !config_.control.room_name.empty();
}

const char* EidolonVoiceController::VoiceStateName(VoiceSessionState state)
{
    switch (state) {
    case VoiceSessionState::Idle:
        return "Idle";
    case VoiceSessionState::PendingApproval:
        return "PendingApproval";
    case VoiceSessionState::WaitingBinding:
        return "WaitingBinding";
    case VoiceSessionState::ConfigReady:
        return "ConfigReady";
    case VoiceSessionState::Connecting:
        return "Connecting";
    case VoiceSessionState::InRoom:
        return "InRoom";
    case VoiceSessionState::Reconnecting:
        return "Reconnecting";
    case VoiceSessionState::Error:
        return "Error";
    case VoiceSessionState::Unauthorized:
        return "Unauthorized";
    case VoiceSessionState::ServerUnreachable:
        return "ServerUnreachable";
    }
    return "Unknown";
}

const char* EidolonVoiceController::CurrentRoomKind() const
{
    if (control_room_) {
        return "control";
    }
    if (session_.IsConnected() || state_ == VoiceSessionState::Connecting ||
        state_ == VoiceSessionState::InRoom || state_ == VoiceSessionState::Reconnecting) {
        return "voice";
    }
    return "none";
}

uint32_t EidolonVoiceController::BeginSessionGeneration(const char* room_kind)
{
    session_generation_ += 1;
    ESP_LOGI(TAG, "[lifecycle] begin gen=%lu room_kind=%s (state=%s)",
             static_cast<unsigned long>(session_generation_), room_kind,
             VoiceStateName(state_));
    return session_generation_;
}

void EidolonVoiceController::MarkSessionSuperseded(const char* reason)
{
    session_generation_ += 1;
    ESP_LOGI(TAG, "[lifecycle] superseded gen=%lu reason=%s (state=%s)",
             static_cast<unsigned long>(session_generation_), reason ? reason : "unspecified",
             VoiceStateName(state_));
}

void EidolonVoiceController::SetState(VoiceSessionState state, const char* reason)
{
    if (state_ == state) {
        return;
    }
    VoiceSessionState prev = state_;
    state_ = state;
    // [lifecycle] is the grep anchor for aligning client transitions with the
    // channel's room-lifecycle logs by room_name + timestamp (plan Phase 0).
    ESP_LOGI(TAG, "[lifecycle] SetState %s -> %s reason=%s room_kind=%s gen=%lu",
             VoiceStateName(prev), VoiceStateName(state), reason ? reason : "unspecified",
             CurrentRoomKind(), static_cast<unsigned long>(session_generation_));
    // The watchdog runs only while an attempt is in flight; any terminal state
    // (InRoom / ConfigReady / Error / ...) disarms it.
    if (state == VoiceSessionState::Connecting || state == VoiceSessionState::Reconnecting) {
        ArmConnectWatchdog();
    } else {
        DisarmConnectWatchdog();
    }
    UpdateIdleAutoLeave();  // arms in-room/idle, disarms on leaving the room
    ResetFullDuplexIdleFallback("state_changed");
    if (on_state_changed_) {
        on_state_changed_(state);
    }
}

esp_err_t EidolonVoiceController::LoadStoredConfig()
{
    HubConfigStore store;
    if (!store.Load(config_, &register_url_)) {
        ESP_LOGE(TAG, "No valid Hub config in NVS");
        SetState(VoiceSessionState::Error, "no_stored_config");
        return ESP_ERR_NOT_FOUND;
    }
    SetState(StateForConfig(config_), "config_loaded");
    return ESP_OK;
}

esp_err_t EidolonVoiceController::RefreshHubConfig(bool persist)
{
    if (register_url_.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    HubConfigClient client;
    Esp32HubConfig fresh;
    // pending_session_intent_ is set only by a proactive room.join and is empty
    // otherwise, so a normal refresh/JOIN sends no intent (Hub → user_initiated).
    esp_err_t err = client.RegisterDevice(register_url_, SystemInfo::GetMacAddress(), fresh,
                                          pending_session_intent_);
    if (err == ESP_ERR_NOT_ALLOWED) {
        // Hub rejected our signed identity (401/403). Stop bouncing on the same
        // rejected key; show "awaiting re-approval" (admin must re-approve / the
        // device must re-enroll). Recovers on a later successful fetch or reboot.
        ESP_LOGW(TAG, "Hub rejected device identity; awaiting re-approval");
        SetState(VoiceSessionState::Unauthorized, "hub_rejected_identity");
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (persist) {
        // The voice room name now carries a per-session nonce, so persisting on
        // every refresh would defeat NVS dedup and wear flash; the per-JOIN
        // refresh passes persist=false and keeps the fresh creds in RAM only.
        HubConfigStore store;
        store.SaveHubConfig(fresh, register_url_);
    }
    config_ = std::move(fresh);
    SetState(StateForConfig(config_), "config_refreshed");
    return ESP_OK;
}

esp_err_t EidolonVoiceController::RediscoverHub()
{
    HubDiscovery discovery;
    HubTxtRecord txt;
    esp_err_t err = discovery.Discover(txt);
    if (err != ESP_OK || txt.register_url.empty()) {
        ESP_LOGW(TAG, "Hub rediscovery failed: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    if (txt.register_url != register_url_) {
        ESP_LOGI(TAG, "Hub address changed: '%s' -> '%s'", register_url_.c_str(),
                 txt.register_url.c_str());
        register_url_ = txt.register_url;
        HubConfigStore store;
        store.SaveTxtRecord(txt);
    }
    // Re-fetch from the (possibly new) URL: server_url/token/control room are all
    // derived from the Hub address and stale if it moved.
    return RefreshHubConfig();
}

// ============================ LiveKit state handler ============================

void EidolonVoiceController::DoLiveKitState(LiveKitConnectionState lk_state,
                                           uint32_t event_generation)
{
    const char* lk_name = LiveKitConnectionStateName(lk_state);
    bool stale = event_generation != session_generation_;
    // [lifecycle] full attribution of every LiveKit event: which plane the
    // controller thinks it is on, the generation the event was emitted under vs
    // the current one (mismatch == a superseded connection's late event leaking
    // in — the cause of the JOIN bounce), the last SDK failure reason, and the
    // room names involved so this aligns with the channel logs.
    ESP_LOGI(TAG,
             "[lifecycle] lk_event=%s room_kind=%s event_gen=%lu cur_gen=%lu%s "
             "state=%s switching=%d failure=%s voice_room=%s control_room=%s",
             lk_name, control_room_ ? "control" : "voice",
             static_cast<unsigned long>(event_generation),
             static_cast<unsigned long>(session_generation_), stale ? " STALE" : "",
             VoiceStateName(state_), switching_to_voice_ ? 1 : 0,
             livekit_failure_reason_str(session_.LastFailureReason()),
             config_.active.room_name.c_str(), config_.control.room_name.c_str());

    // Generation gate (plan §3.2): every connect (voice or control) bumps
    // session_generation_; the callback snapshots the generation it fired under.
    // An event whose generation is not the current one belongs to a superseded
    // connection — e.g. the old control room's late Disconnected/Failed during a
    // JOIN handoff, or a hung attempt we already abandoned. Dropping it here is
    // what stops the InRoom→Ready bounce; it replaces the old single-shot
    // expect_control_teardown_ heuristic, which only masked exactly one
    // Disconnected and missed Failed / reordered / multi-event teardowns.
    if (stale) {
        ESP_LOGI(TAG, "[lifecycle] dropping stale lk_event=%s (event_gen=%lu cur_gen=%lu)",
                 lk_name, static_cast<unsigned long>(event_generation),
                 static_cast<unsigned long>(session_generation_));
        return;
    }

    if (control_room_) {
        StopAudioStatePublisher();
        switch (lk_state) {
        case LiveKitConnectionState::Connecting:
            control_recovery_.OnConnecting();
            ArmConnectWatchdog();
            break;
        case LiveKitConnectionState::Connected:
            control_recovery_.OnConnected();
            if (reconnect_timer_ != nullptr) {
                esp_timer_stop(reconnect_timer_);
            }
            DisarmConnectWatchdog();
#if CONFIG_EIDOLON_GUARD_SERVICE
            FlushPendingGuardPresence();
#endif
            SetState(StateForConfig(config_), "control_connected");
            break;
        case LiveKitConnectionState::Failed:
            control_recovery_.OnDisconnected();
            DisarmConnectWatchdog();
            ESP_LOGW(TAG, "Control room connection failed");
            ScheduleControlReconnect("control_failed");
            break;
        case LiveKitConnectionState::Disconnected:
            control_recovery_.OnDisconnected();
            DisarmConnectWatchdog();
            ScheduleControlReconnect("control_disconnected");
            break;
        case LiveKitConnectionState::Reconnecting:
            // Let the SDK repair a transient ICE/DTLS interruption, but bound the
            // attempt: if it never reaches Connected/Failed/Disconnected, the
            // same watchdog forces the ordinary recovery loop.
            control_recovery_.OnReconnecting();
            ArmConnectWatchdog();
            break;
        }
        return;
    }

    switch (lk_state) {
    case LiveKitConnectionState::Connecting:
        SetState(VoiceSessionState::Connecting, "voice_connecting");
        break;
    case LiveKitConnectionState::Connected:
        control_recovery_.OnVoiceConnected();
        SetState(VoiceSessionState::InRoom, "voice_connected");
        StartAudioStatePublisher();
        if (pending_room_join_command_active_ &&
            (pending_room_join_generation_ == 0 ||
             pending_room_join_generation_ == event_generation)) {
            char result[192];
            snprintf(result, sizeof(result),
                     "{\"status\":\"in_room\",\"room_name\":\"%s\",\"generation\":%lu}",
                     config_.active.room_name.c_str(),
                     static_cast<unsigned long>(event_generation));
            CompletePendingRoomJoinCommand("completed", "OK", "", result);
        }
        break;
    case LiveKitConnectionState::Reconnecting:
        SetState(VoiceSessionState::Reconnecting, "voice_reconnecting");
        break;
    case LiveKitConnectionState::Failed:
        StopAudioStatePublisher();
        agent_phase_ = AgentPhase::Silent;
        CompletePendingRoomJoinCommand(
            "failed", "ROOM_JOIN_FAILED",
            livekit_failure_reason_str(session_.LastFailureReason()));
        if (session_.LastFailureReason() == LIVEKIT_FAILURE_REASON_ROOM_DELETED ||
            session_.LastFailureReason() == LIVEKIT_FAILURE_REASON_ROOM_CLOSED) {
            // [lifecycle] Room Deleted/Closed: the server tore down the voice room.
            // Today the client cannot tell a normal end (idle/proactive done) from a
            // failed join — both land here. Phase 1 distinguishes them via the
            // session_end{reason} packet the channel now sends before deleting.
            ESP_LOGI(TAG,
                     "[lifecycle] voice room closed by server (failure=%s) room=%s; "
                     "returning to control room",
                     livekit_failure_reason_str(session_.LastFailureReason()),
                     config_.active.room_name.c_str());
            SetState(StateForConfig(config_), "voice_room_closed");
            ScheduleControlReconnect("voice_room_closed");
        } else {
            SetState(VoiceSessionState::Error, "voice_failed");
            ScheduleControlReconnect("voice_failed");
        }
        break;
    case LiveKitConnectionState::Disconnected:
        // A control-room teardown during a JOIN handoff is now dropped by the
        // generation gate above (it carries the pre-bump generation), so anything
        // reaching here under the current generation is a genuine voice-room drop.
        StopAudioStatePublisher();
        agent_phase_ = AgentPhase::Silent;
        CompletePendingRoomJoinCommand("failed", "ROOM_JOIN_DISCONNECTED");
        if (state_ != VoiceSessionState::Idle && state_ != VoiceSessionState::ConfigReady &&
            state_ != VoiceSessionState::PendingApproval &&
            state_ != VoiceSessionState::WaitingBinding) {
            SetState(StateForConfig(config_), "voice_disconnected");
        }
        ScheduleControlReconnect("voice_disconnected");
        break;
    }
}

// ============================ Reconnect / watchdog / idle ============================

void EidolonVoiceController::ScheduleControlReconnect(const char* reason)
{
    if (!control_recovery_.TrySchedule(switching_to_voice_, HasControlConfig())) {
        return;
    }
    const int scheduled_attempt = control_recovery_.reconnect_attempts();
    const uint32_t delay_ms = ControlReconnectDelayMs(scheduled_attempt);
    ESP_LOGI(TAG,
             "[lifecycle] control reconnect scheduled reason=%s attempt=%d delay_ms=%lu gen=%lu",
             reason ? reason : "disconnect", scheduled_attempt,
             static_cast<unsigned long>(delay_ms),
             static_cast<unsigned long>(session_generation_));

    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::ReconnectTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_reconnect";
        if (esp_timer_create(&args, &reconnect_timer_) != ESP_OK) {
            reconnect_timer_ = nullptr;
            control_recovery_.OnScheduleFailed();
            ESP_LOGW(TAG, "Failed to create reconnect timer");
            return;
        }
    }
    esp_timer_stop(reconnect_timer_);
    const uint64_t delay_us = static_cast<uint64_t>(delay_ms) * 1000ULL;
    if (esp_timer_start_once(reconnect_timer_, delay_us) != ESP_OK) {
        control_recovery_.OnScheduleFailed();
        ESP_LOGW(TAG, "Failed to start reconnect timer");
    }
}

void EidolonVoiceController::ReconnectTimerCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::ReconnectTick;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoReconnectTick()
{
    int attempt = 0;
    if (!control_recovery_.BeginRetry(&attempt)) {
        return;
    }
    ESP_LOGI(TAG, "[lifecycle] control reconnect tick attempt=%d gen=%lu",
             attempt, static_cast<unsigned long>(session_generation_));
    if (ShouldRediscoverControlConfig(attempt)) {
        // The Hub address may have changed; re-query mDNS and re-fetch config
        // before reconnecting. Authorization/config-status changes terminate
        // this recovery owner instead of reviving stale cached credentials.
        const esp_err_t refresh_err = RediscoverHub();
        if (refresh_err == ESP_ERR_NOT_ALLOWED || !HasControlConfig()) {
            ESP_LOGW(TAG, "Control recovery stopped after credential refresh: %s",
                     esp_err_to_name(refresh_err));
            control_recovery_.FinishRetry();
            return;
        }
    }
    if (ShouldSurfaceControlServerUnreachable(attempt)) {
        SetState(VoiceSessionState::ServerUnreachable, "reconnect_exhausted");
    }

    // Keep the pending guard up across the synchronous connection call. A sync
    // failure is rescheduled below; async terminal events arrive after it clears.
    esp_err_t err = ConnectControlRoom();
    control_recovery_.FinishRetry();
    if (err != ESP_OK) {
        ScheduleControlReconnect("control_retry");
    }
}

void EidolonVoiceController::ArmConnectWatchdog()
{
    if (connect_watchdog_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::ConnectWatchdogCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_conn_wd";
        if (esp_timer_create(&args, &connect_watchdog_) != ESP_OK) {
            connect_watchdog_ = nullptr;
            return;
        }
    }
    esp_timer_stop(connect_watchdog_);  // restart the window for this attempt
    esp_timer_start_once(connect_watchdog_, kConnectWatchdogUs);
}

void EidolonVoiceController::DisarmConnectWatchdog()
{
    if (connect_watchdog_ != nullptr) {
        esp_timer_stop(connect_watchdog_);
    }
}

void EidolonVoiceController::ConnectWatchdogCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::ConnectTimeout;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoConnectTimeout()
{
    // The attempt may have completed between the timer firing and now.
    const bool voice_connecting =
        state_ == VoiceSessionState::Connecting || state_ == VoiceSessionState::Reconnecting;
    const bool control_connecting = control_room_ && control_recovery_.connect_in_flight();
    if (!voice_connecting && !control_connecting) {
        return;
    }
    ESP_LOGW(TAG,
             "[lifecycle] connect watchdog fired state=%s room_kind=%s gen=%lu; forcing recovery",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_));
    StopAudioStatePublisher();
    CompletePendingRoomJoinCommand("failed", "ROOM_JOIN_TIMEOUT");
    // Drop the in-flight guards so ScheduleControlReconnect isn't suppressed, then
    // tear down the hung session and fall back to a stable base state.
    switching_to_voice_ = false;
    control_recovery_.OnDisconnected();
    control_recovery_.FinishRetry();
    session_.Disconnect(true);
    control_room_ = false;
    // Supersede the hung attempt so any of its late callbacks are dropped rather
    // than accepted after we fall back (the next ConnectControlRoom bumps again).
    MarkSessionSuperseded("connect_timeout");
    // leaves (Re)connecting -> disarms watchdog
    SetState(StateForConfig(config_), "connect_timeout");
    ScheduleControlReconnect("connect_timeout");
}

void EidolonVoiceController::UpdateIdleAutoLeave()
{
    if (!ptt_mode_ || kPttIdleFallbackUs == 0) {
        if (idle_leave_timer_ != nullptr) {
            esp_timer_stop(idle_leave_timer_);
        }
        return;
    }
    // Idle = in the voice room, PTT mode, nobody holding the button, and the agent
    // is not producing output. Any of those changing re-evaluates the timer.
    bool agent_output_active = agent_phase_ != AgentPhase::Silent ||
                               local_playback_ui_active_ || PlaybackActiveRecently();
    bool idle = state_ == VoiceSessionState::InRoom && !control_room_ && !ptt_active_ &&
                !agent_output_active;
    if (idle) {
        if (idle_leave_timer_ == nullptr) {
            esp_timer_create_args_t args = {};
            args.callback = &EidolonVoiceController::IdleLeaveCb;
            args.arg = this;
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "eidolon_idle_lv";
            if (esp_timer_create(&args, &idle_leave_timer_) != ESP_OK) {
                idle_leave_timer_ = nullptr;
                return;
            }
        }
        esp_timer_stop(idle_leave_timer_);
        esp_timer_start_once(idle_leave_timer_, kPttIdleFallbackUs);
    } else if (idle_leave_timer_ != nullptr) {
        esp_timer_stop(idle_leave_timer_);
    }
}

void EidolonVoiceController::IdleLeaveCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::IdleLeave;
    self->Enqueue(ev);
}

void EidolonVoiceController::PttReleaseTailCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::PttReleaseTail;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoIdleAutoLeave()
{
    // Activity may have resumed between the timer firing and now.
    bool agent_output_active = agent_phase_ != AgentPhase::Silent ||
                               local_playback_ui_active_ || PlaybackActiveRecently();
    if (!(ptt_mode_ && kPttIdleFallbackUs > 0 && state_ == VoiceSessionState::InRoom &&
          !control_room_ && !ptt_active_ && !agent_output_active)) {
        return;
    }
    ESP_LOGI(TAG,
             "[lifecycle] ptt idle fallback: leaving voice room after %llus idle "
             "room=%s gen=%lu",
             kPttIdleFallbackUs / 1000000ULL, config_.active.room_name.c_str(),
             static_cast<unsigned long>(session_generation_));
    HandleSessionEnd(EndReason::IdleNormalEnd);  // -> control room; re-connect is an explicit tap
}

void EidolonVoiceController::ResetFullDuplexIdleFallback(const char* reason)
{
    if (ptt_mode_ || kFullDuplexIdleFallbackUs == 0) {
        DisarmFullDuplexIdleFallback();
        return;
    }
    if (state_ != VoiceSessionState::InRoom || control_room_ || !session_.IsConnected()) {
        DisarmFullDuplexIdleFallback();
        return;
    }
    if (full_duplex_idle_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::FullDuplexIdleFallbackCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_fd_idle";
        if (esp_timer_create(&args, &full_duplex_idle_timer_) != ESP_OK) {
            full_duplex_idle_timer_ = nullptr;
            ESP_LOGW(TAG, "Failed to create full-duplex idle fallback timer");
            return;
        }
    }
    esp_timer_stop(full_duplex_idle_timer_);
    if (esp_timer_start_once(full_duplex_idle_timer_, kFullDuplexIdleFallbackUs) == ESP_OK) {
        ESP_LOGD(TAG, "[lifecycle] full-duplex idle fallback armed reason=%s timeout=%lums",
                 reason ? reason : "activity",
                 static_cast<unsigned long>(CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS));
    }
}

void EidolonVoiceController::DisarmFullDuplexIdleFallback()
{
    if (full_duplex_idle_timer_ != nullptr) {
        esp_timer_stop(full_duplex_idle_timer_);
    }
}

void EidolonVoiceController::FullDuplexIdleFallbackCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::FullDuplexIdleFallback;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoFullDuplexIdleFallback()
{
    if (ptt_mode_ || state_ != VoiceSessionState::InRoom || control_room_ ||
        !session_.IsConnected()) {
        return;
    }
    if (agent_phase_ != AgentPhase::Silent || AgentOutputActiveRecently()) {
        ResetFullDuplexIdleFallback("activity_still_active");
        return;
    }
    ESP_LOGW(TAG,
             "[lifecycle] full-duplex idle fallback fired after %lums; "
             "no session_end/room-close observed, returning to control room=%s",
             static_cast<unsigned long>(CONFIG_EIDOLON_FULL_DUPLEX_IDLE_FALLBACK_MS),
             config_.active.room_name.c_str());
    HandleSessionEnd(EndReason::IdleNormalEnd);
}

// ============================ Control commands ============================

esp_err_t EidolonVoiceController::AckCommand(const ControlCommand& command, const char* status,
                                             const char* code, const char* detail, const char* result)
{
    const esp_err_t err = session_.PublishData(
        kControlTopic,
        BuildControlAck(command, SystemInfo::GetMacAddress(), status, code, detail, result));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Control ACK publish failed op=%s id=%s status=%s code=%s err=%s",
                 command.op.c_str(), command.id.c_str(), status, code, esp_err_to_name(err));
    }
    return err;
}

void EidolonVoiceController::CompletePendingRoomJoinCommand(const char* status,
                                                            const char* code,
                                                            const char* detail,
                                                            const char* result)
{
    if (!pending_room_join_command_active_) {
        return;
    }
    AckCommand(pending_room_join_command_, status, code, detail, result);
    pending_room_join_command_active_ = false;
    pending_room_join_generation_ = 0;
    pending_room_join_command_ = ControlCommand{};
}

const char* EidolonVoiceController::JoinBlockedCode() const
{
    switch (config_.status) {
    case HubConfigStatus::PendingApproval:
        return "NEEDS_APPROVAL";
    case HubConfigStatus::WaitingBinding:
        return "NEEDS_BINDING";
    case HubConfigStatus::Revoked:
    case HubConfigStatus::Unregistered:
        return "UNAUTHORIZED";
    case HubConfigStatus::Active:
        break;
    }
    return "ROOM_JOIN_FAILED";
}

void EidolonVoiceController::DoControlCommand(const std::string& payload)
{
    ControlCommand command = ParseControlCommand(payload);
    if (!command.valid) {
        ESP_LOGW(TAG, "Ignoring malformed control command");
        return;
    }
    if (command.expired) {
        ESP_LOGW(TAG, "Ignoring expired control command op=%s", command.op.c_str());
        AckCommand(command, "expired", "COMMAND_EXPIRED");
        return;
    }
    // Op dispatch registry. Each handler runs inline on the controller task (no
    // per-command worker task); blocking work just queues other events briefly.
    struct ControlOpHandler {
        const char* op;
        int capability_version;
        void (EidolonVoiceController::*handler)(const std::string&, const std::string&);
    };
    static const ControlOpHandler kControlOps[] = {
        {kControlOpConfigRefresh, 0, &EidolonVoiceController::HandleConfigRefreshCommand},
        {kControlOpRoomJoin, 0, &EidolonVoiceController::HandleRoomJoinCommand},
        {kControlOpPlaybackStop, 0, &EidolonVoiceController::HandlePlaybackStopCommand},
        {kControlOpPttTurnStatus, 0, &EidolonVoiceController::HandlePttTurnStatusCommand},
        {kControlOpDeviceIdentify, 1, &EidolonVoiceController::HandleDeviceIdentifyCommand},
        {kControlOpHeadLookAt, 1, &EidolonVoiceController::HandleHeadLookAtCommand},
        {kControlOpHeadHome, 1, &EidolonVoiceController::HandleHeadHomeCommand},
        {kControlOpHeadGesture, 1, &EidolonVoiceController::HandleHeadGestureCommand},
#if CONFIG_EIDOLON_GUARD_SERVICE
        {kControlOpDeviceRollCall, 1, &EidolonVoiceController::HandleDeviceRollCallCommand},
        {kControlOpGuardRuntimeSync, 0, &EidolonVoiceController::HandleGuardRuntimeSyncCommand},
#endif
#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
        {kControlOpGuardOwnerFaceProfileSync, 0,
         &EidolonVoiceController::HandleGuardOwnerFaceProfileSyncCommand},
#endif
#if CONFIG_EIDOLON_GUARD_VISION_BENCHMARK
        {kControlOpGuardVisionBenchmark, 0,
         &EidolonVoiceController::HandleGuardVisionBenchmarkCommand},
#endif
    };

    for (const auto& entry : kControlOps) {
        if (command.op == entry.op) {
            if (entry.capability_version > 0 && command.capability_version > 0 &&
                command.capability_version != entry.capability_version) {
                ESP_LOGW(TAG,
                         "Unsupported capability version op=%s expected=%d got=%d",
                         command.op.c_str(), entry.capability_version,
                         command.capability_version);
                AckCommand(command, "unsupported", "UNSUPPORTED_CAPABILITY_VERSION");
                return;
            }
            AckCommand(command, "accepted", "OK");
            (this->*entry.handler)(command.id, command.payload);
            return;
        }
    }

    ESP_LOGI(TAG, "Unsupported control command op=%s", command.op.c_str());
    AckCommand(command, "unsupported", "UNSUPPORTED_OP");
}

EndReason EidolonVoiceController::ParseEndReason(const std::string& payload)
{
    // Channel sends {"type":"session_end","reason":"<reason>"} on
    // eidolon.session_control before tearing the room down. Legacy channels sent
    // {"type":"idle_timeout","reason":"idle_timeout"} — treat that as a normal end.
    EndReason reason = EndReason::None;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        return reason;
    }
    cJSON* type_item = cJSON_GetObjectItem(root, "type");
    cJSON* reason_item = cJSON_GetObjectItem(root, "reason");
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    const char* r = cJSON_IsString(reason_item) ? reason_item->valuestring : "";

    if (strcmp(type, kSessionEndType) == 0) {
        if (strcmp(r, kSessionEndIdleNormal) == 0) {
            reason = EndReason::IdleNormalEnd;
        } else if (strcmp(r, kSessionEndProactiveDone) == 0) {
            reason = EndReason::ProactiveDone;
        } else if (strcmp(r, kSessionEndUserLeft) == 0) {
            reason = EndReason::UserLeft;
        } else if (strcmp(r, kSessionEndSuperseded) == 0) {
            reason = EndReason::Superseded;
        } else if (strcmp(r, kSessionEndError) == 0) {
            reason = EndReason::Error;
        } else {
            // Unknown reason from a newer channel: treat as a normal end (return
            // to Ready) rather than guessing an error.
            reason = EndReason::IdleNormalEnd;
        }
    } else if (strcmp(type, "idle_timeout") == 0 || strcmp(r, "idle_timeout") == 0) {
        reason = EndReason::IdleNormalEnd;  // legacy
    }
    cJSON_Delete(root);
    return reason;
}

void EidolonVoiceController::DoSessionControl(const std::string& payload)
{
    EndReason reason = ParseEndReason(payload);
    if (reason == EndReason::None) {
        ESP_LOGI(TAG, "Ignoring unsupported session_control payload");
        return;
    }
    HandleSessionEnd(reason);
}

void EidolonVoiceController::HandleConfigRefreshCommand(const std::string& command_id,
                                                        const std::string& /*payload*/)
{
    ESP_LOGI(TAG, "Control command -> refresh Hub config");
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpConfigRefresh;

    if (RefreshHubConfig() != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered config refresh failed");
        AckCommand(command, "failed", "CONFIG_REFRESH_FAILED");
        return;
    }

    if (config_.status == HubConfigStatus::Active) {
        AckCommand(command, "succeeded", "OK", "", "{\"status\":\"active\"}");
        vTaskDelay(kActiveAckSettleDelay);
        ConnectControlRoom();
        return;
    }

    AckCommand(command, "succeeded", "OK");
    ConnectControlRoom();
}

#if CONFIG_EIDOLON_GUARD_SERVICE
void EidolonVoiceController::HandleGuardRuntimeSyncCommand(const std::string& command_id,
                                                           const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpGuardRuntimeSync;
    if (guard_service_ == nullptr) {
        AckCommand(command, "failed", "GUARD_RUNTIME_UNAVAILABLE");
        return;
    }
    cJSON* root = cJSON_Parse(payload.c_str());
    const cJSON* binding_id = root ? cJSON_GetObjectItem(root, "binding_id") : nullptr;
    const cJSON* revision = root ? cJSON_GetObjectItem(root, "runtime_revision") : nullptr;
    const cJSON* desired = root ? cJSON_GetObjectItem(root, "desired_runtime_state") : nullptr;
    bool only_known_fields = root != nullptr;
    for (const cJSON* item = root ? root->child : nullptr; item != nullptr; item = item->next) {
        if (strcmp(item->string ? item->string : "", "binding_id") != 0 &&
            strcmp(item->string ? item->string : "", "runtime_revision") != 0 &&
            strcmp(item->string ? item->string : "", "desired_runtime_state") != 0) {
            only_known_fields = false;
            break;
        }
    }
    const bool valid = only_known_fields && cJSON_IsString(binding_id) && binding_id->valuestring &&
                       cJSON_IsNumber(revision) && revision->valueint > 0 &&
                       revision->valuedouble == static_cast<double>(revision->valueint) &&
                       cJSON_IsString(desired) && desired->valuestring &&
                       (strcmp(desired->valuestring, "running") == 0 ||
                        strcmp(desired->valuestring, "stopped") == 0);
    if (!valid) {
        cJSON_Delete(root);
        AckCommand(command, "failed", "INVALID_ARGUMENT");
        return;
    }
    const std::string expected_binding_id = binding_id->valuestring;
    const std::string expected_desired_state = desired->valuestring;
    const uint32_t expected_revision = static_cast<uint32_t>(revision->valueint);
    cJSON_Delete(root);

    if (expected_desired_state == "stopped") {
        ClearGuardPresenceRuntime();
        guard_service_->Stop("hub_runtime_stopped");
        has_guard_control_config_ = false;
        guard_control_config_ = RoomConfig{};
        AckCommand(command, "completed", "OK", "",
                   ("{\"binding_id\":\"" + expected_binding_id +
                    "\",\"runtime_revision\":" + std::to_string(expected_revision) +
                    ",\"desired_runtime_state\":\"stopped\",\"running\":false}").c_str());
        vTaskDelay(kActiveAckSettleDelay);
        if (RefreshHubConfig(/*persist=*/false) == ESP_OK) {
            ConnectControlRoom();
        }
        return;
    }

    uint32_t applied_revision = 0;
    if (SyncGuardRuntime("hub_runtime_sync", &expected_binding_id, expected_revision,
                         &expected_desired_state, &applied_revision) != ESP_OK) {
        AckCommand(command, "failed", "GUARD_RUNTIME_SYNC_FAILED");
        return;
    }
    AckCommand(command, "completed", "OK", "",
               ("{\"binding_id\":\"" + expected_binding_id +
                "\",\"runtime_revision\":" + std::to_string(applied_revision) +
                ",\"desired_runtime_state\":\"" + expected_desired_state + "\",\"running\":" +
                (guard_service_->IsRunning() ? "true" : "false") + "}").c_str());
    vTaskDelay(kActiveAckSettleDelay);
    ConnectControlRoom();
}
#endif

#if CONFIG_EIDOLON_OWNER_FACE_PROFILE
void EidolonVoiceController::HandleGuardOwnerFaceProfileSyncCommand(
    const std::string& command_id, const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpGuardOwnerFaceProfileSync;
    OwnerFaceEngine* engine =
        guard_service_ != nullptr ? guard_service_->owner_face_engine() : nullptr;
    if (engine == nullptr || register_url_.empty()) {
        AckCommand(command, "failed", "OWNER_FACE_UNAVAILABLE");
        return;
    }

    cJSON* root = cJSON_Parse(payload.c_str());
    const cJSON* binding_id = root ? cJSON_GetObjectItem(root, "binding_id") : nullptr;
    const cJSON* profile_id = root ? cJSON_GetObjectItem(root, "profile_id") : nullptr;
    const cJSON* revision = root ? cJSON_GetObjectItem(root, "profile_revision") : nullptr;
    const cJSON* desired = root ? cJSON_GetObjectItem(root, "desired_state") : nullptr;
    int field_count = 0;
    int binding_count = 0;
    int profile_count = 0;
    int revision_count = 0;
    int desired_count = 0;
    bool only_known_fields = root != nullptr && cJSON_IsObject(root);
    for (const cJSON* item = root ? root->child : nullptr; item != nullptr; item = item->next) {
        ++field_count;
        const char* key = item->string ? item->string : "";
        if (strcmp(key, "binding_id") == 0) {
            ++binding_count;
        } else if (strcmp(key, "profile_id") == 0) {
            ++profile_count;
        } else if (strcmp(key, "profile_revision") == 0) {
            ++revision_count;
        } else if (strcmp(key, "desired_state") == 0) {
            ++desired_count;
        } else {
            only_known_fields = false;
        }
    }
    const bool integer_revision = cJSON_IsNumber(revision) && revision->valuedouble >= 1 &&
                                  revision->valuedouble <= UINT32_MAX &&
                                  std::floor(revision->valuedouble) == revision->valuedouble;
    const bool valid = only_known_fields && field_count == 4 && binding_count == 1 &&
                       profile_count == 1 && revision_count == 1 && desired_count == 1 &&
                       cJSON_IsString(binding_id) && binding_id->valuestring != nullptr &&
                       binding_id->valuestring[0] != '\0' && strlen(binding_id->valuestring) <= 64 &&
                       cJSON_IsString(profile_id) && profile_id->valuestring != nullptr &&
                       profile_id->valuestring[0] != '\0' && strlen(profile_id->valuestring) <= 64 &&
                       integer_revision && cJSON_IsString(desired) &&
                       desired->valuestring != nullptr &&
                       (strcmp(desired->valuestring, "active") == 0 ||
                        strcmp(desired->valuestring, "cleared") == 0);
    if (!valid) {
        cJSON_Delete(root);
        AckCommand(command, "failed", "INVALID_ARGUMENT");
        return;
    }
    OwnerFaceSyncRequest request = {
        .binding_id = binding_id->valuestring,
        .profile_id = profile_id->valuestring,
        .profile_revision = static_cast<uint32_t>(revision->valuedouble),
        .desired_state = desired->valuestring,
    };
    cJSON_Delete(root);

    const bool queued = engine->QueueSync(
        request, register_url_, SystemInfo::GetMacAddress(),
        [this, command_id](const OwnerFaceApplyResult& result) {
            cJSON* completion = cJSON_CreateObject();
            if (completion == nullptr) {
                ESP_LOGE(TAG, "Owner Face completion allocation failed id=%s", command_id.c_str());
                return;
            }
            cJSON_AddStringToObject(completion, "command_id", command_id.c_str());
            cJSON_AddStringToObject(completion, "binding_id", result.request.binding_id.c_str());
            cJSON_AddStringToObject(completion, "profile_id", result.request.profile_id.c_str());
            cJSON_AddNumberToObject(completion, "profile_revision", result.request.profile_revision);
            cJSON_AddStringToObject(completion, "desired_state", result.request.desired_state.c_str());
            cJSON_AddBoolToObject(completion, "success", result.success);
            cJSON_AddStringToObject(completion, "code", result.code.c_str());
            cJSON_AddStringToObject(completion, "model_id", result.model_id.c_str());
            cJSON_AddStringToObject(completion, "preprocessing_version",
                                    result.preprocessing_version.c_str());
            cJSON_AddNumberToObject(completion, "template_count", result.template_count);
            char* encoded = cJSON_PrintUnformatted(completion);
            cJSON_Delete(completion);
            if (encoded == nullptr) {
                ESP_LOGE(TAG, "Owner Face completion encoding failed id=%s", command_id.c_str());
                return;
            }
            Event ev;
            ev.type = EventType::OwnerFaceProfileCompleted;
            ev.payload = new (std::nothrow) std::string(encoded);
            cJSON_free(encoded);
            if (ev.payload == nullptr) {
                ESP_LOGE(TAG, "Owner Face completion queue allocation failed id=%s",
                         command_id.c_str());
                return;
            }
            Enqueue(ev);
        });
    if (!queued) {
        AckCommand(command, "failed", "OWNER_FACE_BUSY");
    }
}

void EidolonVoiceController::DoOwnerFaceProfileCompleted(const std::string& payload)
{
    cJSON* root = cJSON_Parse(payload.c_str());
    const cJSON* command_id = root ? cJSON_GetObjectItem(root, "command_id") : nullptr;
    const cJSON* binding_id = root ? cJSON_GetObjectItem(root, "binding_id") : nullptr;
    const cJSON* profile_id = root ? cJSON_GetObjectItem(root, "profile_id") : nullptr;
    const cJSON* revision = root ? cJSON_GetObjectItem(root, "profile_revision") : nullptr;
    const cJSON* desired = root ? cJSON_GetObjectItem(root, "desired_state") : nullptr;
    const cJSON* success = root ? cJSON_GetObjectItem(root, "success") : nullptr;
    const cJSON* code = root ? cJSON_GetObjectItem(root, "code") : nullptr;
    const cJSON* model = root ? cJSON_GetObjectItem(root, "model_id") : nullptr;
    const cJSON* preprocessing =
        root ? cJSON_GetObjectItem(root, "preprocessing_version") : nullptr;
    const cJSON* templates = root ? cJSON_GetObjectItem(root, "template_count") : nullptr;
    if (!cJSON_IsString(command_id) || !cJSON_IsString(binding_id) ||
        !cJSON_IsString(profile_id) || !cJSON_IsNumber(revision) ||
        !cJSON_IsString(desired) || !cJSON_IsBool(success) || !cJSON_IsString(code) ||
        !cJSON_IsString(model) || !cJSON_IsString(preprocessing) ||
        !cJSON_IsNumber(templates)) {
        ESP_LOGE(TAG, "Ignoring malformed Owner Face completion");
        cJSON_Delete(root);
        return;
    }
    ControlCommand command;
    command.id = command_id->valuestring;
    command.op = kControlOpGuardOwnerFaceProfileSync;
    if (!cJSON_IsTrue(success)) {
        AckCommand(command, "failed", code->valuestring);
        cJSON_Delete(root);
        return;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "binding_id", binding_id->valuestring);
    cJSON_AddStringToObject(result, "profile_id", profile_id->valuestring);
    cJSON_AddNumberToObject(result, "profile_revision", revision->valuedouble);
    cJSON_AddStringToObject(result, "applied_state", desired->valuestring);
    if (strcmp(desired->valuestring, "cleared") == 0) {
        cJSON_AddNullToObject(result, "model_id");
        cJSON_AddNullToObject(result, "preprocessing_version");
        cJSON_AddNumberToObject(result, "template_count", 0);
    } else {
        cJSON_AddStringToObject(result, "model_id", model->valuestring);
        cJSON_AddStringToObject(result, "preprocessing_version", preprocessing->valuestring);
        cJSON_AddNumberToObject(result, "template_count", templates->valueint);
    }
    char* encoded = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (encoded == nullptr) {
        AckCommand(command, "failed", "OWNER_FACE_RESULT_ENCODING_FAILED");
        cJSON_Delete(root);
        return;
    }
    AckCommand(command, "succeeded", "OK", "", encoded);
    cJSON_free(encoded);
    cJSON_Delete(root);
}
#endif

void EidolonVoiceController::HandleRoomJoinCommand(const std::string& command_id,
                                                   const std::string& payload)
{
    // A proactive wake (Phase 3) arrives as room.join with a payload that carries
    // session_intent ("proactive_initiated"). Stash it so the JOIN's token-fetch
    // declares it to the Hub (→ token metadata → channel suppresses the welcome).
    // A plain user-initiated room.join has no payload; pending stays empty.
    pending_session_intent_.clear();
    if (!payload.empty()) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root != nullptr) {
            const cJSON* intent = cJSON_GetObjectItem(root, "session_intent");
            if (cJSON_IsString(intent) && intent->valuestring != nullptr) {
                const char* value = intent->valuestring;
                if (strcmp(value, kSessionIntentProactive) == 0) {
                    pending_session_intent_ = value;
                } else {
                    ESP_LOGW(TAG, "Ignoring unsupported session_intent=%s", value);
                }
            }
            cJSON_Delete(root);
        }
    }
    ESP_LOGI(TAG, "Control command -> join voice room (intent=%s)",
             pending_session_intent_.empty() ? "user" : pending_session_intent_.c_str());
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpRoomJoin;
    vTaskDelay(kRoomJoinSettleDelay);

    pending_room_join_command_ = command;
    pending_room_join_command_active_ = true;
    pending_room_join_generation_ = 0;
    esp_err_t err = DoJoinRoom();
    // One-shot: clear after the JOIN (incl. DoJoinRoom's internal connect-retry,
    // which re-fetches) so the intent never leaks into a later reconnect/refresh.
    pending_session_intent_.clear();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered room join failed: %s", esp_err_to_name(err));
        CompletePendingRoomJoinCommand("failed", JoinBlockedCode(), esp_err_to_name(err));
        return;
    }
    pending_room_join_generation_ = session_generation_;
    if (state_ == VoiceSessionState::InRoom) {
        char result[192];
        snprintf(result, sizeof(result),
                 "{\"status\":\"in_room\",\"room_name\":\"%s\",\"generation\":%lu}",
                 config_.active.room_name.c_str(),
                 static_cast<unsigned long>(session_generation_));
        CompletePendingRoomJoinCommand("completed", "OK", "", result);
    }
}

void EidolonVoiceController::HandlePlaybackStopCommand(const std::string& command_id,
                                                       const std::string& /*payload*/)
{
    ESP_LOGI(TAG, "Control command -> stop playback");
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpPlaybackStop;

    esp_err_t flush_err = StopLocalPlayback("control_playback_stop");
    if (flush_err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered playback stop failed: %s", esp_err_to_name(flush_err));
        AckCommand(command, "failed", "PLAYBACK_FLUSH_FAILED", esp_err_to_name(flush_err));
        return;
    }

    PublishClientAudioState(false);
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandlePttTurnStatusCommand(const std::string& command_id,
                                                        const std::string& payload)
{
    std::string outcome;
    if (!payload.empty()) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root != nullptr) {
            const cJSON* item = cJSON_GetObjectItem(root, "outcome");
            if (cJSON_IsString(item) && item->valuestring != nullptr) {
                outcome = item->valuestring;
            }
            cJSON_Delete(root);
        }
    }
    if (outcome.empty()) {
        outcome = "unknown";
    }
    ESP_LOGI(TAG, "PTT turn status outcome=%s", outcome.c_str());
    if (on_ptt_turn_status_) {
        on_ptt_turn_status_(outcome);
    }

    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpPttTurnStatus;
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleDeviceIdentifyCommand(const std::string& command_id,
                                                         const std::string& /*payload*/)
{
    ESP_LOGI(TAG, "Control command -> identify device");
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpDeviceIdentify;

    if (!control_room_) {
        ESP_LOGW(TAG, "Identify ignored outside control room");
        AckCommand(command, "failed", "IDENTIFY_REQUIRES_CONTROL_ROOM");
        return;
    }

    esp_err_t tone_err = PlayIdentifyFeedback();
    if (tone_err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered identify tone failed: %s", esp_err_to_name(tone_err));
        AckCommand(command, "failed", "IDENTIFY_TONE_FAILED", esp_err_to_name(tone_err));
        return;
    }

    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleHeadLookAtCommand(const std::string& command_id,
                                                     const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpHeadLookAt;

    auto& board = Board::GetInstance();
    if (!board.HasHeadMotion()) {
        AckCommand(command, "failed", "NO_HEAD_MOTION");
        return;
    }

    float x = 0.0f, y = 0.0f;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root) {
        const cJSON* jx = cJSON_GetObjectItem(root, "x");
        const cJSON* jy = cJSON_GetObjectItem(root, "y");
        if (cJSON_IsNumber(jx)) x = static_cast<float>(jx->valuedouble);
        if (cJSON_IsNumber(jy)) y = static_cast<float>(jy->valuedouble);
        cJSON_Delete(root);
    }
    // Normalized inputs; the motion layer maps to the mechanical range and clamps.
    if (x < -1.0f) x = -1.0f; else if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f; else if (y > 1.0f) y = 1.0f;

    ESP_LOGI(TAG, "Control command -> head.look_at x=%.2f y=%.2f", x, y);
    board.HeadLookAt(x, y);
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleHeadHomeCommand(const std::string& command_id,
                                                   const std::string& /*payload*/)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpHeadHome;

    auto& board = Board::GetInstance();
    if (!board.HasHeadMotion()) {
        AckCommand(command, "failed", "NO_HEAD_MOTION");
        return;
    }
    ESP_LOGI(TAG, "Control command -> head.home");
    board.HeadHome();
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleHeadGestureCommand(const std::string& command_id,
                                                      const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpHeadGesture;

    auto& board = Board::GetInstance();
    if (!board.HasHeadMotion()) {
        AckCommand(command, "failed", "NO_HEAD_MOTION");
        return;
    }

    std::string name;
    int times = 0, hold_ms = 0, return_ms = 0;
    float x = 0.0f, y = 0.0f;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root) {
        const cJSON* jn = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(jn) && jn->valuestring) name = jn->valuestring;
        const cJSON* jt = cJSON_GetObjectItem(root, "times");
        if (cJSON_IsNumber(jt)) times = jt->valueint;
        const cJSON* jh = cJSON_GetObjectItem(root, "hold_ms");
        if (cJSON_IsNumber(jh)) hold_ms = jh->valueint;
        const cJSON* jr = cJSON_GetObjectItem(root, "return_ms");
        if (cJSON_IsNumber(jr)) return_ms = jr->valueint;
        const cJSON* jx = cJSON_GetObjectItem(root, "x");
        if (cJSON_IsNumber(jx)) x = static_cast<float>(jx->valuedouble);
        const cJSON* jy = cJSON_GetObjectItem(root, "y");
        if (cJSON_IsNumber(jy)) y = static_cast<float>(jy->valuedouble);
        cJSON_Delete(root);
    }
    if (name.empty()) {
        AckCommand(command, "failed", "MISSING_GESTURE_NAME");
        return;
    }
    if (x < -1.0f) x = -1.0f; else if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f; else if (y > 1.0f) y = 1.0f;

    ESP_LOGI(TAG, "Control command -> head.gesture %s", name.c_str());
    board.HeadGesture(name, times, x, y, hold_ms, return_ms);
    AckCommand(command, "completed", "OK");
}

#if CONFIG_EIDOLON_GUARD_SERVICE
void EidolonVoiceController::HandleDeviceRollCallCommand(const std::string& command_id,
                                                         const std::string& /*payload*/)
{
    ESP_LOGI(TAG, "Control command -> Guard roll call");
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpDeviceRollCall;

    if (!control_room_) {
        AckCommand(command, "failed", "ROLL_CALL_REQUIRES_CONTROL_ROOM");
        return;
    }
    const esp_err_t feedback_err = PlayRollCallFeedback();
    if (feedback_err != ESP_OK) {
        AckCommand(command, "failed", "ROLL_CALL_PLAYBACK_FAILED",
                   esp_err_to_name(feedback_err));
        return;
    }
    AckCommand(command, "completed", "OK", "", "{\"played\":true}");
}
#endif

#if CONFIG_EIDOLON_GUARD_VISION_BENCHMARK
void EidolonVoiceController::HandleGuardVisionBenchmarkCommand(const std::string& command_id,
                                                                const std::string& payload)
{
    ControlCommand command;
    command.id = command_id;
    command.op = kControlOpGuardVisionBenchmark;

    if (!control_room_) {
        AckCommand(command, "failed", "VISION_PROBE_REQUIRES_CONTROL_ROOM");
        return;
    }

    int sample_count = 30;
    int interval_ms = 300;
    cJSON* root = payload.empty() ? nullptr : cJSON_Parse(payload.c_str());
    if (!payload.empty() && root == nullptr) {
        AckCommand(command, "failed", "INVALID_ARGUMENT", "invalid JSON payload");
        return;
    }
    if (root != nullptr) {
        const cJSON* samples = cJSON_GetObjectItem(root, "sample_count");
        const cJSON* interval = cJSON_GetObjectItem(root, "interval_ms");
        if ((samples != nullptr && !cJSON_IsNumber(samples)) ||
            (interval != nullptr && !cJSON_IsNumber(interval))) {
            cJSON_Delete(root);
            AckCommand(command, "failed", "INVALID_ARGUMENT", "sample_count and interval_ms must be integers");
            return;
        }
        if (samples != nullptr) {
            sample_count = samples->valueint;
        }
        if (interval != nullptr) {
            interval_ms = interval->valueint;
        }
        cJSON_Delete(root);
    }
    if (sample_count < 10 || sample_count > 120 || interval_ms < 100 || interval_ms > 2000) {
        AckCommand(command, "failed", "INVALID_ARGUMENT", "sample_count=10..120, interval_ms=100..2000");
        return;
    }

    Camera* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) {
        AckCommand(command, "failed", "CAMERA_UNAVAILABLE");
        return;
    }

    ESP_LOGI(TAG, "Control command -> guard vision benchmark samples=%d interval_ms=%d",
             sample_count, interval_ms);
    const std::string result = GuardVisionBenchmark::Run(*camera, sample_count, interval_ms);
    AckCommand(command, "completed", "OK", "", result.c_str());
}
#endif

void EidolonVoiceController::HandleIdleTimeoutCommand()
{
    // Retained for the legacy idle path; routed through the unified handler.
    HandleSessionEnd(EndReason::IdleNormalEnd);
}

void EidolonVoiceController::HandleSessionEnd(EndReason reason)
{
    static const char* kReasonNames[] = {"none",
                                         kSessionEndIdleNormal,
                                         kSessionEndProactiveDone,
                                         kSessionEndUserLeft,
                                         kSessionEndSuperseded,
                                         kSessionEndError};
    const char* reason_name = kReasonNames[static_cast<int>(reason)];
    // Record the reason so the UI can show "已结束待命" / error chrome instead of an
    // unexplained return to JOIN, then tear the voice room down gracefully and
    // fall back to the (always-on) control room for reachability. The device is
    // still reachable on control even after an error, so we do NOT force the
    // Error connection state; the reason drives the UI text orthogonally (§3.2).
    last_end_reason_ = reason;
    ESP_LOGI(TAG,
             "[lifecycle] session_end reason=%s; leaving voice for control (gen=%lu room=%s)",
             reason_name, static_cast<unsigned long>(session_generation_),
             config_.active.room_name.c_str());

    StopAudioStatePublisher();
    control_recovery_.OnIntentionalTeardown();
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    session_.Disconnect(true);
    control_room_ = false;
    // Supersede the torn-down voice room so its trailing ROOM_DELETED/Disconnected
    // (which the server emits right after this packet) is dropped by the
    // generation gate instead of being read as a fresh voice-room drop.
    MarkSessionSuperseded("session_end");
    SetState(StateForConfig(config_), "session_end");
    ConnectControlRoom();
}

// ============================ Lifecycle handlers ============================

void EidolonVoiceController::DoActivation()
{
    control_recovery_.OnActivation();
    // Wire the SDK callbacks to post events so all state mutation stays on the loop.
    session_.SetOnStateChanged([this](LiveKitConnectionState s, uint32_t generation) {
        Event ev;
        ev.type = EventType::LiveKitState;
        ev.lk_state = s;
        // LiveKitSession binds this value when the room is created. Never read
        // the controller's newer generation from the SDK callback task: a late
        // teardown must retain ownership by the room that emitted it.
        ev.generation = generation;
        Enqueue(ev);
    });
    session_.SetOnControlCommand([this](const std::string& payload) {
        Event ev;
        ev.type = EventType::ControlCommand;
        ev.payload = new std::string(payload);
        Enqueue(ev);
    });
    session_.SetOnSessionControl([this](const std::string& payload) {
        Event ev;
        ev.type = EventType::SessionControl;
        ev.payload = new std::string(payload);
        Enqueue(ev);
    });

    if (LoadStoredConfig() != ESP_OK) {
        return;
    }

    if (!HasActiveConfig()) {
        esp_err_t refresh_err = RefreshHubConfig();
        if (refresh_err != ESP_OK) {
            ESP_LOGW(TAG, "Initial Hub config refresh failed: %s", esp_err_to_name(refresh_err));
        }
    }
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_ != nullptr) {
        const esp_err_t guard_err = SyncGuardRuntime("activation");
        if (guard_err != ESP_OK && guard_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Initial Guard runtime sync failed: %s", esp_err_to_name(guard_err));
        }
    }
#endif

    // PTT (half-duplex): land in the ready state on the lightweight control room —
    // entering the voice room (which plays the welcome) is an explicit tap, so the
    // device never boots straight into an open session. Full-duplex may auto-join
    // the voice room on activation (open mic) when the board opts in.
#if CONFIG_EIDOLON_AUTO_JOIN_ON_ACTIVATION
    if (!ptt_mode_ && HasActiveConfig()) {
        DoJoinRoom();
        return;
    }
#endif
    if (HasControlConfig()) {
        ConnectControlRoom();
    }
}

void EidolonVoiceController::DoNetworkLost()
{
    ESP_LOGW(TAG,
             "[lifecycle] network_lost executing state=%s room_kind=%s gen=%lu "
             "connected=%d voice_room=%s control_room=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0,
             config_.active.room_name.c_str(), config_.control.room_name.c_str());
    StopAudioStatePublisher();
    control_recovery_.OnNetworkLost();
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_ != nullptr) {
        // Presence facts are meaningful only while a signed Guard runtime is
        // current. Do not retain local observations across a network boundary.
        ClearGuardPresenceRuntime();
        guard_service_->Stop("network_lost");
    }
#endif
    control_room_ = false;
    session_.Disconnect(true);
    // Supersede so late callbacks from the dropped connection don't resurrect a
    // stale state once the network returns and we reconnect.
    MarkSessionSuperseded("network_lost");
    SetState(StateForConfig(config_), "network_lost");
}

void EidolonVoiceController::DoNetworkRestored()
{
    ESP_LOGI(TAG, "Network restored; refreshing Hub discovery/config");
    control_recovery_.OnNetworkRestored();
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }

    if (register_url_.empty() && LoadStoredConfig() != ESP_OK) {
        ESP_LOGW(TAG, "Network restored but no stored Hub config is available");
        return;
    }

    esp_err_t err = RediscoverHub();
    if (err == ESP_ERR_NOT_ALLOWED) {
        ESP_LOGW(TAG, "Network restore stopped: Hub rejected device identity");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hub rediscovery after network restore failed: %s", esp_err_to_name(err));
        err = RefreshHubConfig();
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        ESP_LOGW(TAG, "Network restore stopped: Hub rejected device identity");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hub config refresh after network restore failed: %s", esp_err_to_name(err));
        ScheduleControlReconnect("network_restore_refresh_failed");
        return;
    }

#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_ != nullptr) {
        const esp_err_t guard_err = SyncGuardRuntime("network_restore");
        if (guard_err != ESP_OK && guard_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Guard runtime sync after network restore failed: %s", esp_err_to_name(guard_err));
        }
    }
#endif

    if (HasControlConfig()) {
        err = ConnectControlRoom();
        if (err != ESP_OK) {
            ScheduleControlReconnect("network_restore_connect_failed");
        }
        return;
    }

    SetState(StateForConfig(config_), "network_restored");
}

esp_err_t EidolonVoiceController::DoJoinRoom()
{
    ESP_LOGI(TAG,
             "[lifecycle] join executing state=%s room_kind=%s gen=%lu control_room=%d "
             "connected=%d voice_room=%s control_room_name=%s intent=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), control_room_ ? 1 : 0,
             session_.IsConnected() ? 1 : 0, config_.active.room_name.c_str(),
             config_.control.room_name.c_str(),
             pending_session_intent_.empty() ? "user" : pending_session_intent_.c_str());
    if (state_ == VoiceSessionState::Connecting || state_ == VoiceSessionState::Reconnecting) {
        ESP_LOGI(TAG, "[lifecycle] join ignored while session is already transitioning state=%s",
                 VoiceStateName(state_));
        return ESP_ERR_INVALID_STATE;
    }
    if (state_ == VoiceSessionState::InRoom) {
        ESP_LOGI(TAG, "[lifecycle] join ignored: already in voice room=%s gen=%lu",
                 config_.active.room_name.c_str(),
                 static_cast<unsigned long>(session_generation_));
        return ESP_OK;
    }

    if (!config_.active.usable()) {
        if (LoadStoredConfig() != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    // Every JOIN mints a FRESH per-session voice room (device-<id>-<nonce>) +
    // scoped token by re-fetching the Hub config. Root-cause fix for the
    // rapid-rejoin "Room Deleted" race: a previous session's late delete-by-name
    // (the old agent deletes device-<id> on device-left/shutdown) can no longer
    // tear down this session's uniquely-named room. RAM-only (persist=false) so
    // the changing nonce does not wear NVS. Done BEFORE moving to Connecting so
    // RefreshHubConfig's internal SetState(ConfigReady) stays a no-op.
    esp_err_t refresh_err = RefreshHubConfig(/*persist=*/false);
    if (refresh_err == ESP_ERR_NOT_ALLOWED) {
        // Hub revoked this identity; RefreshHubConfig already surfaced
        // Unauthorized. Don't attempt to join on a rejected key.
        return refresh_err;
    }
    if (refresh_err != ESP_OK) {
        // Hub unreachable: fall back to the cached room+token — degraded (reuses
        // the last room, so the delete race can reappear) but keeps JOIN working
        // offline. The race only bites under Hub-up churn anyway.
        ESP_LOGW(TAG, "Join: Hub config refresh failed (%s); using cached room=%s",
                 esp_err_to_name(refresh_err), config_.active.room_name.c_str());
    }
    if (!HasActiveConfig()) {
        ESP_LOGW(TAG, "Join blocked: config status=%s",
                 HubConfigStatusToString(config_.status));
        SetState(StateForConfig(config_), "join_blocked_inactive");
        return refresh_err != ESP_OK ? refresh_err : ESP_ERR_INVALID_STATE;
    }

    switching_to_voice_ = true;
    control_recovery_.OnIntentionalTeardown();
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    // Fresh session: drop any end reason from the previous conversation so the
    // connecting/ready chrome doesn't show a stale "已结束".
    last_end_reason_ = EndReason::None;
    SetState(VoiceSessionState::Connecting, "join_requested");
    if (control_room_) {
        // Tear down the control room first. Its Disconnected/Failed events are
        // emitted here, still under the current (control) generation; we bump to
        // the voice generation only afterwards, so those late events are dropped
        // by the generation gate in DoLiveKitState rather than misread as a
        // voice-room drop. Disconnect() is synchronous (waits for DISCONNECTED +
        // a short grace), so they are enqueued before the bump below.
        session_.Disconnect(true);
    }
    control_room_ = false;
    uint32_t voice_generation = BeginSessionGeneration("voice");
    esp_err_t err = session_.Connect(config_, voice_generation);
    switching_to_voice_ = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connect failed, refreshing Hub token");
        if (RefreshHubConfig(/*persist=*/false) == ESP_OK) {
            switching_to_voice_ = true;
            voice_generation = BeginSessionGeneration("voice");
            err = session_.Connect(config_, voice_generation);
            switching_to_voice_ = false;
        }
    }
    if (err != ESP_OK) {
        // Connect() may have emitted Connecting before failing synchronously.
        // Retire this generation so that queued tail event cannot re-arm the
        // voice watchdog after we have already chosen the control fallback.
        MarkSessionSuperseded("voice_connect_sync_failed");
        SetState(VoiceSessionState::Error, "join_connect_failed");
        ScheduleControlReconnect("voice_connect_sync_failed");
    }
    return err;
}

esp_err_t EidolonVoiceController::ConnectControlRoom()
{
    if (!control_recovery_.network_available() || !HasControlConfig()) {
        return ESP_ERR_INVALID_STATE;
    }

    // ConnectDataOnly connects to control_config.active. While pending/waiting
    // that is already the pending room; once active, point it at the control room
    // (falling back to the active identity when the control room omits one).
    const RoomConfig* runtime_fallback = nullptr;
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (has_guard_control_config_) {
        runtime_fallback = &guard_control_config_;
    }
#endif
    Esp32HubConfig control_config = BuildControlConnectionConfig(config_, runtime_fallback);

    // Retire any old room before assigning the next generation. Disconnect is
    // synchronous in LiveKitSession; the explicit supersede generation also
    // makes callbacks already in flight stale before the new attempt begins.
    if (session_.HasRoom()) {
        MarkSessionSuperseded("control_attempt_replace");
        session_.Disconnect(true);
    }
    control_room_ = true;
    const uint32_t control_generation = BeginSessionGeneration("control");
    control_recovery_.OnAttemptStarted();
    esp_err_t err = session_.ConnectDataOnly(control_config, control_generation);
    if (err != ESP_OK) {
        // Keep control_room_ true: it denotes the plane owned by this generation,
        // not connection health. Superseding drops any Connecting/Disconnected
        // callback queued before the synchronous failure was returned.
        control_recovery_.OnDisconnected();
        MarkSessionSuperseded("control_connect_sync_failed");
        ESP_LOGW(TAG, "Connect control room failed: %s", esp_err_to_name(err));
    }
    // ServerUnreachable remains visible until LiveKit actually reports Connected.
    if (state_ != VoiceSessionState::ServerUnreachable) {
        SetState(StateForConfig(config_), "control_connect");
    }
    if (err == ESP_OK) {
        ArmConnectWatchdog();
    } else {
        ScheduleControlReconnect("control_connect_sync_failed");
    }
    return err;
}

#if CONFIG_EIDOLON_GUARD_SERVICE
esp_err_t EidolonVoiceController::SyncGuardRuntime(const char* reason,
                                                    const std::string* expected_binding_id,
                                                    uint32_t expected_runtime_revision,
                                                    const std::string* expected_desired_state,
                                                    uint32_t* applied_runtime_revision)
{
    if (guard_service_ == nullptr || register_url_.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    GuardRuntimeHubConfig runtime;
    HubConfigClient client;
    const esp_err_t err =
        client.FetchGuardRuntime(register_url_, SystemInfo::GetMacAddress(), runtime);
    if (err == ESP_ERR_NOT_FOUND) {
        ClearGuardPresenceRuntime();
        guard_service_->Stop("guard_binding_missing");
        has_guard_control_config_ = false;
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }
    if ((expected_binding_id && runtime.binding_id != *expected_binding_id) ||
        (expected_runtime_revision > runtime.runtime_revision) ||
        (expected_desired_state && runtime.desired_runtime_state != *expected_desired_state)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (applied_runtime_revision != nullptr) {
        *applied_runtime_revision = runtime.runtime_revision;
    }
    has_guard_control_config_ = true;
    guard_control_config_ = runtime.control;
    if (runtime.desired_runtime_state == "stopped") {
        guard_service_->Stop(reason);
        ClearGuardPresenceRuntime();
        has_guard_control_config_ = false;
        guard_control_config_ = RoomConfig{};
        return ESP_OK;
    }
    GuardRuntimeConfig config;
    config.sample_interval_ms = runtime.sample_interval_ms;
    config.preview_interval_ms = runtime.preview_interval_ms;
    config.motion_threshold = runtime.motion_threshold;
    config.motion_clear_threshold = runtime.motion_clear_threshold;
    config.candidate_debounce_ms = runtime.candidate_debounce_ms;
    config.absence_timeout_ms = runtime.absence_timeout_ms;
    config.consecutive_capture_failures = runtime.consecutive_capture_failures;
    config.owner_face_interval_ms = runtime.owner_face_interval_ms;
    config.owner_presence_enter_ms = runtime.owner_presence_enter_ms;
    config.owner_presence_exit_ms = runtime.owner_presence_exit_ms;
    config.owner_presence_heartbeat_ms = runtime.owner_presence_heartbeat_ms;
    config.owner_presence_lease_ms = runtime.owner_presence_lease_ms;
    ConfigureGuardPresenceRuntime(runtime);
    return guard_service_->Start(config, reason) ? ESP_OK : ESP_FAIL;
}

void EidolonVoiceController::ConfigureGuardPresenceRuntime(const GuardRuntimeHubConfig& runtime)
{
    if (guard_service_ == nullptr) {
        return;
    }
    ++guard_runtime_generation_;
    pending_guard_presence_payloads_.clear();
    const uint32_t boot_nonce = esp_random();
    guard_presence_adapter_.Configure({
        .guard_companion_id = runtime.guard_companion_id,
        .device_id = SystemInfo::GetMacAddress(),
        .runtime_revision = runtime.runtime_revision,
        .candidate_debounce_ms = runtime.candidate_debounce_ms,
        .boot_nonce = boot_nonce,
    });
    owner_presence_adapter_.Configure({
        .guard_companion_id = runtime.guard_companion_id,
        .device_id = SystemInfo::GetMacAddress(),
        .boot_nonce = boot_nonce,
    });
    const uint32_t generation = guard_runtime_generation_;
    guard_service_->SetObservationCallback([this, generation](const GuardObservation& observation) {
        Event ev;
        ev.type = EventType::GuardObservation;
        ev.generation = generation;
        ev.guard_observation = observation;
        Enqueue(ev);
    });
    guard_service_->SetOwnerPresenceCallback(
        [this, generation](const OwnerPresenceObservation& observation) {
            Event ev;
            ev.type = EventType::OwnerPresence;
            ev.generation = generation;
            ev.owner_presence_observation = observation;
            Enqueue(ev);
        });
}

void EidolonVoiceController::ClearGuardPresenceRuntime()
{
    ++guard_runtime_generation_;
    pending_guard_presence_payloads_.clear();
    guard_presence_adapter_.Clear();
    owner_presence_adapter_.Clear();
#if CONFIG_EIDOLON_GUARD_SERVICE
    if (guard_service_ != nullptr) {
        guard_service_->SetObservationCallback({});
        guard_service_->SetOwnerPresenceCallback({});
    }
#endif
}

void EidolonVoiceController::DoGuardObservation(const GuardObservation& observation,
                                                 uint32_t runtime_generation)
{
    if (runtime_generation != guard_runtime_generation_) {
        ESP_LOGD(TAG, "Dropped stale Guard observation epoch=%lu generation=%lu",
                 static_cast<unsigned long>(observation.epoch),
                 static_cast<unsigned long>(runtime_generation));
        return;
    }
    auto payload = guard_presence_adapter_.Build(
        observation, GuardEventTimestampMs(observation.now_ms));
    if (!payload.has_value()) {
        return;
    }
    if (pending_guard_presence_payloads_.size() >= kMaxPendingGuardPresenceEvents) {
        ESP_LOGW(TAG, "Dropped Guard fact: volatile queue is full epoch=%lu state=%s",
                 static_cast<unsigned long>(observation.epoch), GuardStateName(observation.state));
        return;
    }
    pending_guard_presence_payloads_.push_back(std::move(*payload));
    FlushPendingGuardPresence();
}

void EidolonVoiceController::DoOwnerPresence(
    const OwnerPresenceObservation& observation, uint32_t runtime_generation)
{
    if (runtime_generation != guard_runtime_generation_) {
        ESP_LOGD(TAG, "Dropped stale Owner Presence fact epoch=%lu generation=%lu",
                 static_cast<unsigned long>(observation.epoch),
                 static_cast<unsigned long>(runtime_generation));
        return;
    }
    auto payload = owner_presence_adapter_.Build(
        observation, GuardEventTimestampMs(observation.now_ms));
    if (!payload.has_value()) {
        return;
    }
    if (pending_guard_presence_payloads_.size() >= kMaxPendingGuardPresenceEvents) {
        ESP_LOGW(TAG, "Dropped Owner Presence fact: volatile queue is full epoch=%lu",
                 static_cast<unsigned long>(observation.epoch));
        return;
    }
    pending_guard_presence_payloads_.push_back(std::move(*payload));
    FlushPendingGuardPresence();
}

void EidolonVoiceController::FlushPendingGuardPresence()
{
    if (!has_guard_control_config_ || !control_room_ || !session_.IsConnected()) {
        return;
    }
    while (!pending_guard_presence_payloads_.empty()) {
        const esp_err_t err = session_.PublishData(kControlTopic,
                                                   pending_guard_presence_payloads_.front());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Guard fact publish deferred: %s", esp_err_to_name(err));
            return;
        }
        pending_guard_presence_payloads_.pop_front();
    }
}

uint64_t EidolonVoiceController::GuardEventTimestampMs(uint64_t monotonic_ms)
{
    const std::time_t seconds = std::time(nullptr);
    // The signed config route intentionally supports pre-SNTP devices. Preserve
    // a useful ordering timestamp in that case, but never treat it as wall clock.
    if (seconds >= 1'700'000'000) {
        return static_cast<uint64_t>(seconds) * 1000ULL;
    }
    return monotonic_ms;
}
#endif

esp_err_t EidolonVoiceController::DoLeaveRoom()
{
    bool playback_recent = PlaybackActiveRecently();
    ESP_LOGI(TAG,
             "[lifecycle] leave executing state=%s room_kind=%s gen=%lu connected=%d "
             "control_room=%d voice_room=%s control_room_name=%s ptt_active=%d tail=%d "
             "audio_pub=%d playback_recent=%d local_playback=%d agent=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0,
             control_room_ ? 1 : 0, config_.active.room_name.c_str(),
             config_.control.room_name.c_str(), ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0, audio_publisher_active_ ? 1 : 0,
             playback_recent ? 1 : 0, local_playback_ui_active_ ? 1 : 0,
             AgentPhaseName(agent_phase_));
    if (!control_room_) {
        esp_err_t stop_err = StopLocalPlayback("leave_room");
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "Leave-room playback stop skipped: %s", esp_err_to_name(stop_err));
        }
    }
    StopAudioStatePublisher();
    bool reconnect_control = HasControlConfig();
    esp_err_t err = session_.Disconnect(true);
    control_room_ = false;
    if (reconnect_control) {
        ConnectControlRoom();
    } else if (state_ != VoiceSessionState::Idle) {
        SetState(StateForConfig(config_), "leave_room");
    }
    ESP_LOGI(TAG,
             "[lifecycle] leave complete err=%s reconnect_control=%d state=%s room_kind=%s "
             "gen=%lu",
             esp_err_to_name(err), reconnect_control ? 1 : 0, VoiceStateName(state_),
             CurrentRoomKind(), static_cast<unsigned long>(session_generation_));
    return err;
}

// ============================ Audio state publisher ============================

void EidolonVoiceController::StartAudioStatePublisher()
{
    if (audio_publisher_active_) {
        return;
    }
    audio_state_seq_ = 0;
    audio_state_sent_ = false;
    local_playback_ui_active_ = false;
    audio_publisher_active_ = true;
    if (audio_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::AudioTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_audio";
        if (esp_timer_create(&args, &audio_timer_) != ESP_OK) {
            audio_timer_ = nullptr;
            audio_publisher_active_ = false;
            ESP_LOGW(TAG, "Failed to create audio state timer");
            return;
        }
    }
    esp_timer_start_periodic(audio_timer_, kAudioTickIntervalUs);
}

void EidolonVoiceController::StopAudioStatePublisher()
{
    if (!audio_publisher_active_) {
        return;
    }
    audio_publisher_active_ = false;
    if (audio_timer_ != nullptr) {
        esp_timer_stop(audio_timer_);
    }
    if (ptt_mode_) {
        CancelPttReleaseTail();
        ptt_active_ = false;
    }
    // Emit a final closed-mic state (matches the old publisher's exit behavior).
    if (session_.IsConnected() && !control_room_) {
        PublishClientAudioState(false);
    }
}

void EidolonVoiceController::AudioTimerCb(void* arg)
{
    auto* self = static_cast<EidolonVoiceController*>(arg);
    Event ev;
    ev.type = EventType::AudioTick;
    self->Enqueue(ev);
}

void EidolonVoiceController::DoAudioTick()
{
    if (!audio_publisher_active_ || control_room_ || !session_.IsConnected()) {
        return;
    }
    PublishClientAudioState(AgentOutputActiveRecently());
}

void EidolonVoiceController::PublishClientAudioState(bool playback_active)
{
    char payload[320];
    int64_t now_us = esp_timer_get_time();
    uint32_t client_ts_ms = static_cast<uint32_t>(now_us / 1000);

    bool ptt_held = ptt_mode_ && ptt_active_;
    bool mic_muted;
    bool capture_on;
    if (ptt_mode_) {
        // Push-to-talk (half-duplex): the mic is open while held and during the
        // short release tail. Closed otherwise, so playback is not recorded and
        // the ptt=false edge remains the explicit "I'm done" turn boundary.
        capture_on = mic_enabled_ && ptt_active_;
        mic_muted = !capture_on;
    } else {
        // Auto open-mic (full-duplex): keep capture open during playback so
        // device-side AEC can support barge-in.
        capture_on = mic_enabled_;
        mic_muted = !mic_enabled_;
    }

    // Physically gate the capture path to match so no unwanted audio reaches the
    // channel.
    eidolon_livekit_board_set_capture_enabled(capture_on);
    uint32_t capture_rms_ppm = eidolon_livekit_board_recent_capture_rms_ppm();
    if (mic_muted) {
        capture_rms_ppm = 0;
    }
    uint32_t playback_rms_ppm = eidolon_livekit_board_recent_playback_rms_ppm();

    bool state_changed = !audio_state_sent_ ||
                         playback_active != last_audio_playback_active_ ||
                         mic_muted != last_audio_mic_muted_ ||
                         ptt_held != last_audio_ptt_;
    UpdateLocalPlaybackPhase(playback_active);
    // Fast poll, throttled heartbeat: nothing changed and the heartbeat isn't due
    // yet, so don't emit a packet (last_audio_* already equals the current state).
    if (!state_changed && (now_us - last_audio_publish_us_) < kAudioStateHeartbeatUs) {
        return;
    }
    uint32_t seq = ++audio_state_seq_;
    unsigned long capture_rms_whole =
        static_cast<unsigned long>(capture_rms_ppm / 1000000UL);
    unsigned long capture_rms_frac =
        static_cast<unsigned long>(capture_rms_ppm % 1000000UL);
    int written = snprintf(
        payload,
        sizeof(payload),
        "{\"schema_v\":%d,\"type\":\"%s\",\"seq\":%lu,\"input_mode\":\"%s\","
        "\"playback_state\":\"%s\",\"mic_muted\":%s,"
        "\"ptt\":%s,\"rms\":%lu.%06lu,\"client_ts_ms\":%lu}",
        kWireSchemaVersion,
        kClientAudioStateType,
        static_cast<unsigned long>(seq),
        ptt_mode_ ? kInputModePtt : kInputModeAuto,
        playback_active ? kPlaybackStateAgentSpeaking : kPlaybackStateIdle,
        mic_muted ? "true" : "false",
        ptt_held ? "true" : "false",
        capture_rms_whole,
        capture_rms_frac,
        static_cast<unsigned long>(client_ts_ms));
    if (written <= 0 || written >= static_cast<int>(sizeof(payload))) {
        return;
    }
    esp_err_t err = session_.PublishData(kClientAudioStateTopic, payload, state_changed);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Publish client audio state skipped: %s", esp_err_to_name(err));
        return;
    }
    if (state_changed || ptt_held || playback_active) {
        ESP_LOGI(TAG,
                 "Audio state seq=%lu mode=%s playback=%s mic_muted=%d ptt=%d "
                 "capture_rms=%lu.%03lu playback_rms=%lu.%03lu",
                 static_cast<unsigned long>(seq),
                 ptt_mode_ ? kInputModePtt : kInputModeAuto,
                 playback_active ? kPlaybackStateAgentSpeaking : kPlaybackStateIdle,
                 mic_muted ? 1 : 0,
                 ptt_held ? 1 : 0,
                 static_cast<unsigned long>(capture_rms_ppm / 1000000UL),
                 static_cast<unsigned long>((capture_rms_ppm % 1000000UL) / 1000UL),
                 static_cast<unsigned long>(playback_rms_ppm / 1000000UL),
                 static_cast<unsigned long>((playback_rms_ppm % 1000000UL) / 1000UL));
    }
    audio_state_sent_ = true;
    last_audio_playback_active_ = playback_active;
    last_audio_mic_muted_ = mic_muted;
    last_audio_ptt_ = ptt_held;
    last_audio_publish_us_ = now_us;
}

bool EidolonVoiceController::PlaybackActiveRecently() const
{
    int64_t last_playback_us = eidolon_livekit_board_last_playback_us();
    int64_t now_us = esp_timer_get_time();
    return last_playback_us > 0 && (now_us - last_playback_us) <= kPlaybackActiveWindowUs;
}

bool EidolonVoiceController::AgentOutputActiveRecently() const
{
    if (agent_phase_ == AgentPhase::AgentSpeaking) {
        return true;
    }
    return PlaybackActiveRecently();
}

void EidolonVoiceController::UpdateLocalPlaybackPhase(bool playback_active)
{
    if (!ptt_mode_ || control_room_ || state_ != VoiceSessionState::InRoom) {
        return;
    }
    if (local_playback_ui_active_ == playback_active) {
        return;
    }
    local_playback_ui_active_ = playback_active;

    if (playback_active) {
        if (on_agent_phase_) {
            on_agent_phase_(AgentPhase::AgentSpeaking);
        }
    } else if (agent_phase_ != AgentPhase::AgentThinking &&
               agent_phase_ != AgentPhase::AgentSpeaking) {
        if (on_agent_phase_) {
            on_agent_phase_(AgentPhase::Silent);
        }
    }
    UpdateIdleAutoLeave();
}

esp_err_t EidolonVoiceController::StopLocalPlayback(const char* reason)
{
    esp_err_t flush_err = eidolon_livekit_board_flush_playback();
    if (flush_err != ESP_OK) {
        return flush_err;
    }
    ESP_LOGI(TAG, "Local playback stopped reason=%s", reason ? reason : "unspecified");
    UpdateLocalPlaybackPhase(false);
    DoAgentPhase(AgentPhase::Silent);
    return ESP_OK;
}

// ============================ Mic / PTT / agent phase ============================

void EidolonVoiceController::DoSetMicEnabled(bool enabled)
{
    mic_enabled_ = enabled;
    if (session_.IsConnected() && !control_room_) {
        PublishClientAudioState(AgentOutputActiveRecently());
    } else {
        eidolon_livekit_board_set_capture_enabled(enabled);
    }
    ESP_LOGI(TAG, "Mic enabled=%d", enabled ? 1 : 0);
}

void EidolonVoiceController::DoPttPressed()
{
    if (!ptt_mode_) {
        return;
    }
    bool resumed_from_tail = ptt_release_tail_pending_;
    bool playback_recent = PlaybackActiveRecently();
    ESP_LOGI(TAG,
             "[ptt] press executing state=%s room_kind=%s gen=%lu connected=%d "
             "control_room=%d ptt_active=%d tail=%d playback_recent=%d local_playback=%d "
             "agent=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0,
             control_room_ ? 1 : 0, ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0, playback_recent ? 1 : 0,
             local_playback_ui_active_ ? 1 : 0, AgentPhaseName(agent_phase_));
    CancelPttReleaseTail();
    ptt_active_ = true;
    UpdateIdleAutoLeave();  // active: cancel the idle countdown
    // Hold-to-talk only applies in-room. Entering the room is an explicit tap (the
    // talk button is a "connect" button until connected) — a press while not
    // in-room is ignored rather than silently joining and dropping the first words.
    if (state_ != VoiceSessionState::InRoom) {
        ptt_active_ = false;
        ESP_LOGI(TAG, "[ptt] press ignored: not in room (tap to connect first) state=%s",
                 VoiceStateName(state_));
        return;
    }
    ESP_LOGI(TAG, "%s", resumed_from_tail ? "[ptt] press: release tail cancelled, mic open"
                                          : "[ptt] press: mic open");
    PublishClientAudioState(AgentOutputActiveRecently());
}

void EidolonVoiceController::DoPttReleased()
{
    if (!ptt_mode_) {
        return;
    }
    ESP_LOGI(TAG,
             "[ptt] release executing state=%s room_kind=%s gen=%lu connected=%d "
             "control_room=%d ptt_active=%d tail=%d playback_recent=%d local_playback=%d "
             "agent=%s",
             VoiceStateName(state_), CurrentRoomKind(),
             static_cast<unsigned long>(session_generation_), session_.IsConnected() ? 1 : 0,
             control_room_ ? 1 : 0, ptt_active_ ? 1 : 0,
             ptt_release_tail_pending_ ? 1 : 0, PlaybackActiveRecently() ? 1 : 0,
             local_playback_ui_active_ ? 1 : 0, AgentPhaseName(agent_phase_));
    if (!ptt_active_ && !ptt_release_tail_pending_) {
        ESP_LOGI(TAG, "[ptt] release ignored: not active");
        return;
    }
    if (!session_.IsConnected() || control_room_ || state_ != VoiceSessionState::InRoom) {
        FinalizePttRelease("not_in_voice_room");
        return;
    }
    if (kPttReleaseTailUs == 0) {
        FinalizePttRelease("no_tail");
        return;
    }
    if (ptt_release_tail_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::PttReleaseTailCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_ptt_tail";
        if (esp_timer_create(&args, &ptt_release_tail_timer_) != ESP_OK) {
            ptt_release_tail_timer_ = nullptr;
            ESP_LOGW(TAG, "Failed to create PTT release tail timer");
            FinalizePttRelease("tail_timer_create_failed");
            return;
        }
    }
    ptt_release_tail_pending_ = true;
    esp_timer_stop(ptt_release_tail_timer_);
    if (esp_timer_start_once(ptt_release_tail_timer_, kPttReleaseTailUs) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start PTT release tail timer");
        FinalizePttRelease("tail_timer_start_failed");
        return;
    }
    ESP_LOGI(TAG, "[ptt] release: keeping mic open for tail=%lums",
             static_cast<unsigned long>(CONFIG_EIDOLON_PTT_RELEASE_TAIL_MS));
}

void EidolonVoiceController::DoPttReleaseTail()
{
    if (!ptt_mode_ || !ptt_release_tail_pending_) {
        return;
    }
    FinalizePttRelease("tail_elapsed");
}

void EidolonVoiceController::CancelPttReleaseTail()
{
    if (ptt_release_tail_timer_ != nullptr) {
        esp_timer_stop(ptt_release_tail_timer_);
    }
    ptt_release_tail_pending_ = false;
}

void EidolonVoiceController::FinalizePttRelease(const char* reason)
{
    CancelPttReleaseTail();
    ptt_active_ = false;
    if (session_.IsConnected() && !control_room_) {
        ESP_LOGI(TAG, "[ptt] release: mic closed, turn committed (%s)",
                 reason ? reason : "unknown");
        PublishClientAudioState(AgentOutputActiveRecently());
    } else {
        ESP_LOGI(TAG, "[ptt] release: mic closed without publish (%s)",
                 reason ? reason : "not_connected");
        eidolon_livekit_board_set_capture_enabled(false);
    }
    UpdateIdleAutoLeave();  // turn done: start the idle countdown (if agent silent)
}

void EidolonVoiceController::DoAgentPhase(AgentPhase phase)
{
    bool changed = phase != agent_phase_;
    agent_phase_ = phase;
    if (changed) {
        ESP_LOGI(TAG, "Agent phase -> %s", AgentPhaseName(phase));
        if (session_.IsConnected() && !control_room_) {
            PublishClientAudioState(AgentOutputActiveRecently());
        }
        UpdateIdleAutoLeave();  // agent busy disarms; back to silent arms the timer
        ResetFullDuplexIdleFallback("agent_phase");
    }
    if (on_agent_phase_) {
        on_agent_phase_(phase);
    }
}

void EidolonVoiceController::DoSessionActivity()
{
    ResetFullDuplexIdleFallback("transcription");
}

}  // namespace eidolon
