#include "eidolon_voice_controller.h"

#include "board.h"
#include "control_protocol.h"
#include "hub_config_client.h"
#include "hub_config_store.h"
#include "hub_discovery.h"
#include "livekit_board.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>

#define TAG "EidolonVoice"

namespace {
constexpr const char* kClientAudioStateTopic = "eidolon.audio_state";
constexpr int64_t kPlaybackActiveWindowUs = 800 * 1000;
// Poll the audio state fast so a barge-in (near-end speech) edge reaches the
// channel within ~one poll, but only emit an unchanged heartbeat every
// kAudioStateHeartbeatUs to avoid flooding lossy packets at the poll rate.
constexpr uint64_t kAudioTickIntervalUs = 80 * 1000;
constexpr int64_t kAudioStateHeartbeatUs = 500 * 1000;
// Reconnect backoff: the Nth attempt waits min(1s << N, 30s) so transient
// blips recover fast while a down/moved Hub is not hammered.
constexpr uint32_t kReconnectBaseDelayMs = 1000;
constexpr uint32_t kReconnectMaxDelayMs = 30000;
// After this many consecutive failures, re-query mDNS before each reconnect —
// the Hub IP may have changed (DHCP / network move), making the cached address
// dead. The first attempt stays fast (cached address) for the common transient
// case.
constexpr int kReconnectRediscoverAfter = 1;
// After this many consecutive failures, surface "cannot reach server" in the UI
// (keep retrying in the background so it self-heals when the Hub returns).
constexpr int kReconnectUnreachableAfter = 2;
// A connect/reconnect attempt that never reaches a terminal LiveKit state within
// this long is treated as hung and force-recovered. Generous enough to cover a
// slow mDNS + HTTPS + connect on a healthy-but-slow network.
constexpr uint64_t kConnectWatchdogUs = 25ULL * 1000 * 1000;
// PTT: leave the voice room after this long in-room with no PTT activity and no
// agent output, so the server can release the agent session. Re-connecting is an
// explicit tap. Tunable; 30-45s feels responsive without churning on short pauses.
constexpr uint64_t kIdleAutoLeaveUs = 40ULL * 1000 * 1000;
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

uint64_t ReconnectDelayUs(int attempt)
{
    uint32_t ms = kReconnectBaseDelayMs;
    for (int i = 0; i < attempt && ms < kReconnectMaxDelayMs; ++i) {
        ms <<= 1;
    }
    if (ms > kReconnectMaxDelayMs) {
        ms = kReconnectMaxDelayMs;
    }
    return static_cast<uint64_t>(ms) * 1000ULL;
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

EidolonVoiceController::EidolonVoiceController()
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
         {&audio_timer_, &reconnect_timer_, &connect_watchdog_, &idle_leave_timer_}) {
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
    case EventType::LiveKitState:
        DoLiveKitState(ev.lk_state);
        break;
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
    Event ev;
    ev.type = EventType::NetworkLost;
    Enqueue(ev);
}

esp_err_t EidolonVoiceController::JoinRoom()
{
    Event ev;
    ev.type = EventType::Join;
    Enqueue(ev);
    return ESP_OK;
}

esp_err_t EidolonVoiceController::LeaveRoom()
{
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
    Event ev;
    ev.type = EventType::PttPress;
    Enqueue(ev);
}

void EidolonVoiceController::OnPttReleased()
{
    Event ev;
    ev.type = EventType::PttRelease;
    Enqueue(ev);
}

void EidolonVoiceController::SetOnTranscription(std::function<void(const TranscriptionEvent&)> cb)
{
    // Transcription goes straight to the UI (Application schedules it) and never
    // touches controller state, so it does not need to go through the loop.
    session_.SetOnTranscription(std::move(cb));
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
    return config_.status == HubConfigStatus::Active && config_.active.usable();
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

void EidolonVoiceController::SetState(VoiceSessionState state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    ESP_LOGI(TAG, "Voice session state -> %d", static_cast<int>(state));
    // The watchdog runs only while an attempt is in flight; any terminal state
    // (InRoom / ConfigReady / Error / ...) disarms it.
    if (state == VoiceSessionState::Connecting || state == VoiceSessionState::Reconnecting) {
        ArmConnectWatchdog();
    } else {
        DisarmConnectWatchdog();
    }
    UpdateIdleAutoLeave();  // arms in-room/idle, disarms on leaving the room
    if (on_state_changed_) {
        on_state_changed_(state);
    }
}

esp_err_t EidolonVoiceController::LoadStoredConfig()
{
    HubConfigStore store;
    if (!store.Load(config_, &config_url_)) {
        ESP_LOGE(TAG, "No valid Hub config in NVS");
        SetState(VoiceSessionState::Error);
        return ESP_ERR_NOT_FOUND;
    }
    SetState(StateForConfig(config_));
    return ESP_OK;
}

esp_err_t EidolonVoiceController::RefreshHubConfig()
{
    if (config_url_.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    HubConfigClient client;
    Esp32HubConfig fresh;
    esp_err_t err = client.Fetch(config_url_, SystemInfo::GetMacAddress(), fresh);
    if (err != ESP_OK) {
        return err;
    }
    HubConfigStore store;
    store.SaveHubConfig(fresh, config_url_);
    config_ = std::move(fresh);
    SetState(StateForConfig(config_));
    return ESP_OK;
}

esp_err_t EidolonVoiceController::RediscoverHub()
{
    HubDiscovery discovery;
    HubTxtRecord txt;
    esp_err_t err = discovery.Discover(txt);
    if (err != ESP_OK || txt.config_url.empty()) {
        ESP_LOGW(TAG, "Hub rediscovery failed: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    if (txt.config_url != config_url_) {
        ESP_LOGI(TAG, "Hub address changed: '%s' -> '%s'", config_url_.c_str(),
                 txt.config_url.c_str());
        config_url_ = txt.config_url;
        HubConfigStore store;
        store.SaveTxtRecord(txt);
    }
    // Re-fetch from the (possibly new) URL: server_url/token/control room are all
    // derived from the Hub address and stale if it moved.
    return RefreshHubConfig();
}

// ============================ LiveKit state handler ============================

void EidolonVoiceController::DoLiveKitState(LiveKitConnectionState lk_state)
{
    if (control_room_) {
        StopAudioStatePublisher();
        switch (lk_state) {
        case LiveKitConnectionState::Connected:
            reconnect_attempts_ = 0;  // recovered: control room is back
            SetState(StateForConfig(config_));
            break;
        case LiveKitConnectionState::Failed:
            ESP_LOGW(TAG, "Control room connection failed");
            ScheduleControlReconnect("control_failed");
            break;
        case LiveKitConnectionState::Disconnected:
            ScheduleControlReconnect("control_disconnected");
            break;
        case LiveKitConnectionState::Connecting:
        case LiveKitConnectionState::Reconnecting:
            // Mid-attempt: keep the current status. Don't clobber a shown
            // ServerUnreachable with a transient "ready"; Connected clears it,
            // the reconnect path escalates it.
            break;
        }
        return;
    }

    switch (lk_state) {
    case LiveKitConnectionState::Connecting:
        SetState(VoiceSessionState::Connecting);
        break;
    case LiveKitConnectionState::Connected:
        reconnect_attempts_ = 0;  // recovered: voice room is up
        SetState(VoiceSessionState::InRoom);
        StartAudioStatePublisher();
        break;
    case LiveKitConnectionState::Reconnecting:
        SetState(VoiceSessionState::Reconnecting);
        break;
    case LiveKitConnectionState::Failed:
        StopAudioStatePublisher();
        agent_phase_ = AgentPhase::Silent;
        if (session_.LastFailureReason() == LIVEKIT_FAILURE_REASON_ROOM_DELETED ||
            session_.LastFailureReason() == LIVEKIT_FAILURE_REASON_ROOM_CLOSED) {
            ESP_LOGI(TAG, "Voice room closed by server, returning to control room");
            SetState(StateForConfig(config_));
            ScheduleControlReconnect("voice_room_closed");
        } else {
            SetState(VoiceSessionState::Error);
            ScheduleControlReconnect("voice_failed");
        }
        break;
    case LiveKitConnectionState::Disconnected:
        StopAudioStatePublisher();
        agent_phase_ = AgentPhase::Silent;
        if (state_ != VoiceSessionState::Idle && state_ != VoiceSessionState::ConfigReady &&
            state_ != VoiceSessionState::PendingApproval &&
            state_ != VoiceSessionState::WaitingBinding) {
            SetState(StateForConfig(config_));
        }
        ScheduleControlReconnect("voice_disconnected");
        break;
    }
}

// ============================ Reconnect / watchdog / idle ============================

void EidolonVoiceController::ScheduleControlReconnect(const char* reason)
{
    if (control_room_ || switching_to_voice_ || control_reconnect_pending_ || !HasControlConfig()) {
        return;
    }
    control_reconnect_pending_ = true;
    ESP_LOGI(TAG, "Scheduling control room reconnect after %s (attempt %d)",
             reason ? reason : "disconnect", reconnect_attempts_);

    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &EidolonVoiceController::ReconnectTimerCb;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "eidolon_reconnect";
        if (esp_timer_create(&args, &reconnect_timer_) != ESP_OK) {
            reconnect_timer_ = nullptr;
            control_reconnect_pending_ = false;
            ESP_LOGW(TAG, "Failed to create reconnect timer");
            return;
        }
    }
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, ReconnectDelayUs(reconnect_attempts_));
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
    int attempt = reconnect_attempts_;
    if (attempt >= kReconnectRediscoverAfter) {
        // The Hub address may have changed; re-query mDNS and re-fetch config
        // before reconnecting (best-effort, errors are logged).
        RediscoverHub();
    }
    if (attempt >= kReconnectUnreachableAfter) {
        SetState(VoiceSessionState::ServerUnreachable);
    }
    reconnect_attempts_ = attempt + 1;

    // Keep the guard up across ConnectControlRoom so its internal synchronous
    // Disconnect can't trigger a spurious re-schedule; clear it afterward so the
    // (async) Failed callback drives the next attempt, and if the connect failed
    // synchronously, drive it ourselves.
    esp_err_t err = ConnectControlRoom();
    control_reconnect_pending_ = false;
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
    if (state_ != VoiceSessionState::Connecting && state_ != VoiceSessionState::Reconnecting) {
        return;
    }
    ESP_LOGW(TAG, "Connect watchdog fired (stuck in state %d); forcing reconnect",
             static_cast<int>(state_));
    StopAudioStatePublisher();
    // Drop the in-flight guards so ScheduleControlReconnect isn't suppressed, then
    // tear down the hung session and fall back to a stable base state.
    switching_to_voice_ = false;
    control_reconnect_pending_ = false;
    session_.Disconnect(true);
    control_room_ = false;
    SetState(StateForConfig(config_));  // leaves (Re)connecting -> disarms watchdog
    ScheduleControlReconnect("connect_timeout");
}

void EidolonVoiceController::UpdateIdleAutoLeave()
{
    // Idle = in the voice room, PTT mode, nobody holding the button, and the agent
    // is not producing output. Any of those changing re-evaluates the timer.
    bool idle = ptt_mode_ && state_ == VoiceSessionState::InRoom && !control_room_ &&
                !ptt_active_ && agent_phase_ == AgentPhase::Silent;
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
        esp_timer_start_once(idle_leave_timer_, kIdleAutoLeaveUs);
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

void EidolonVoiceController::DoIdleAutoLeave()
{
    // Activity may have resumed between the timer firing and now.
    if (!(ptt_mode_ && state_ == VoiceSessionState::InRoom && !control_room_ &&
          !ptt_active_ && agent_phase_ == AgentPhase::Silent)) {
        return;
    }
    ESP_LOGI(TAG, "Idle auto-leave: leaving voice room after %llus idle",
             kIdleAutoLeaveUs / 1000000ULL);
    DoLeaveRoom();  // -> control room / ConfigReady; re-connect is an explicit tap
}

// ============================ Control commands ============================

void EidolonVoiceController::AckCommand(const ControlCommand& command, const char* status,
                                        const char* code, const char* detail, const char* result)
{
    session_.PublishData(
        kControlTopic,
        BuildControlAck(command, SystemInfo::GetMacAddress(), status, code, detail, result));
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
        void (EidolonVoiceController::*handler)(const std::string&);
    };
    static const ControlOpHandler kControlOps[] = {
        {"config.refresh", &EidolonVoiceController::HandleConfigRefreshCommand},
        {"room.join", &EidolonVoiceController::HandleRoomJoinCommand},
        {"playback.stop", &EidolonVoiceController::HandlePlaybackStopCommand},
    };

    for (const auto& entry : kControlOps) {
        if (command.op == entry.op) {
            AckCommand(command, "accepted", "OK");
            (this->*entry.handler)(command.id);
            return;
        }
    }

    ESP_LOGI(TAG, "Unsupported control command op=%s", command.op.c_str());
    AckCommand(command, "unsupported", "UNSUPPORTED_OP");
}

void EidolonVoiceController::DoSessionControl(const std::string& payload)
{
    if (payload.find("idle_timeout") == std::string::npos) {
        ESP_LOGI(TAG, "Ignoring unsupported session_control payload");
        return;
    }
    ESP_LOGI(TAG, "Session control -> idle timeout");
    HandleIdleTimeoutCommand();
}

void EidolonVoiceController::HandleConfigRefreshCommand(const std::string& command_id)
{
    ESP_LOGI(TAG, "Control command -> refresh Hub config");
    ControlCommand command;
    command.id = command_id;
    command.op = "config.refresh";

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

void EidolonVoiceController::HandleRoomJoinCommand(const std::string& command_id)
{
    ESP_LOGI(TAG, "Control command -> join voice room");
    ControlCommand command;
    command.id = command_id;
    command.op = "room.join";
    vTaskDelay(kRoomJoinSettleDelay);

    esp_err_t err = DoJoinRoom();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered room join failed: %s", esp_err_to_name(err));
        // Reply with a failed ACK so the caller isn't left waiting on the earlier
        // "accepted". Success stays implicit: the connection completes async and
        // surfaces as the InRoom state, not a synchronous "completed" here.
        AckCommand(command, "failed", "ROOM_JOIN_FAILED", esp_err_to_name(err));
    }
}

void EidolonVoiceController::HandlePlaybackStopCommand(const std::string& command_id)
{
    ESP_LOGI(TAG, "Control command -> stop playback");
    ControlCommand command;
    command.id = command_id;
    command.op = "playback.stop";

    esp_err_t flush_err = eidolon_livekit_board_flush_playback();
    if (flush_err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered playback stop failed: %s", esp_err_to_name(flush_err));
        AckCommand(command, "failed", "PLAYBACK_FLUSH_FAILED", esp_err_to_name(flush_err));
        return;
    }

    DoAgentPhase(AgentPhase::Silent);
    PublishClientAudioState(false);
    AckCommand(command, "completed", "OK");
}

void EidolonVoiceController::HandleIdleTimeoutCommand()
{
    StopAudioStatePublisher();
    control_reconnect_pending_ = true;
    session_.Disconnect(true);
    control_room_ = false;
    SetState(StateForConfig(config_));
    control_reconnect_pending_ = false;
    ConnectControlRoom();
}

// ============================ Lifecycle handlers ============================

void EidolonVoiceController::DoActivation()
{
    // Wire the SDK callbacks to post events so all state mutation stays on the loop.
    session_.SetOnStateChanged([this](LiveKitConnectionState s) {
        Event ev;
        ev.type = EventType::LiveKitState;
        ev.lk_state = s;
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
    StopAudioStatePublisher();
    control_room_ = false;
    reconnect_attempts_ = 0;  // distinct cause; reconnect starts fresh on restore
    session_.Disconnect(true);
    SetState(StateForConfig(config_));
}

esp_err_t EidolonVoiceController::DoJoinRoom()
{
    if (state_ == VoiceSessionState::Connecting || state_ == VoiceSessionState::Reconnecting) {
        ESP_LOGI(TAG, "Join ignored while session is already transitioning");
        return ESP_ERR_INVALID_STATE;
    }
    if (state_ == VoiceSessionState::InRoom) {
        return ESP_OK;
    }

    if (!config_.active.usable()) {
        if (LoadStoredConfig() != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    if (!HasActiveConfig()) {
        ESP_LOGI(TAG, "Config status=%s, refreshing before join",
                 HubConfigStatusToString(config_.status));
        SetState(VoiceSessionState::Connecting);
        esp_err_t refresh_err = RefreshHubConfig();
        if (refresh_err != ESP_OK) {
            ESP_LOGW(TAG, "Join blocked: config refresh failed: %s", esp_err_to_name(refresh_err));
            SetState(StateForConfig(config_));
            return refresh_err;
        }
        if (!HasActiveConfig()) {
            ESP_LOGW(TAG, "Join blocked: config status=%s",
                     HubConfigStatusToString(config_.status));
            SetState(StateForConfig(config_));
            return ESP_ERR_INVALID_STATE;
        }
    }

    switching_to_voice_ = true;
    SetState(VoiceSessionState::Connecting);
    if (control_room_) {
        session_.Disconnect(true);
    }
    control_room_ = false;
    esp_err_t err = session_.Connect(config_);
    switching_to_voice_ = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connect failed, refreshing Hub token");
        if (RefreshHubConfig() == ESP_OK) {
            switching_to_voice_ = true;
            err = session_.Connect(config_);
            switching_to_voice_ = false;
        }
    }
    if (err != ESP_OK) {
        SetState(VoiceSessionState::Error);
    }
    return err;
}

esp_err_t EidolonVoiceController::ConnectControlRoom()
{
    if (!HasControlConfig()) {
        return ESP_ERR_INVALID_STATE;
    }

    // ConnectDataOnly connects to control_config.active. While pending/waiting
    // that is already the pending room; once active, point it at the control room
    // (falling back to the active identity when the control room omits one).
    Esp32HubConfig control_config = config_;
    if (config_.status == HubConfigStatus::Active) {
        control_config.active = config_.control;
        if (control_config.active.identity.empty()) {
            control_config.active.identity = config_.active.identity;
        }
    }

    control_room_ = true;
    esp_err_t err = session_.ConnectDataOnly(control_config);
    if (err != ESP_OK) {
        control_room_ = false;
        ESP_LOGW(TAG, "Connect control room failed: %s", esp_err_to_name(err));
    }
    SetState(StateForConfig(config_));
    return err;
}

esp_err_t EidolonVoiceController::DoLeaveRoom()
{
    StopAudioStatePublisher();
    bool reconnect_control = HasControlConfig();
    esp_err_t err = session_.Disconnect(true);
    control_room_ = false;
    if (reconnect_control) {
        ConnectControlRoom();
    } else if (state_ != VoiceSessionState::Idle) {
        SetState(StateForConfig(config_));
    }
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
    char payload[280];
    int64_t now_us = esp_timer_get_time();
    uint32_t client_ts_ms = static_cast<uint32_t>(now_us / 1000);

    bool ptt_held = ptt_mode_ && ptt_active_;
    bool mic_muted;
    bool capture_on;
    if (ptt_mode_) {
        // Push-to-talk (half-duplex): the mic is open ONLY while the button is
        // held. Closed otherwise, so playback is never recorded and the turn
        // boundary is explicit (the ptt false edge tells the server "I'm done").
        capture_on = mic_enabled_ && ptt_active_;
        mic_muted = !capture_on;
    } else {
        // Auto open-mic (full-duplex): the mic is closed only while the agent is
        // speaking (no usable barge-in echo cancellation on this board).
        capture_on = mic_enabled_ && !playback_active;
        mic_muted = !mic_enabled_ || playback_active;
    }

    // Physically gate the capture path to match so no unwanted audio reaches the
    // channel.
    eidolon_livekit_board_set_capture_enabled(capture_on);

    bool state_changed = !audio_state_sent_ ||
                         playback_active != last_audio_playback_active_ ||
                         mic_muted != last_audio_mic_muted_ ||
                         ptt_held != last_audio_ptt_;
    // Fast poll, throttled heartbeat: nothing changed and the heartbeat isn't due
    // yet, so don't emit a packet (last_audio_* already equals the current state).
    if (!state_changed && (now_us - last_audio_publish_us_) < kAudioStateHeartbeatUs) {
        return;
    }
    uint32_t seq = ++audio_state_seq_;
    int written = snprintf(
        payload,
        sizeof(payload),
        "{\"type\":\"client.audio_state\",\"seq\":%lu,\"input_mode\":\"%s\","
        "\"playback_state\":\"%s\",\"mic_muted\":%s,"
        "\"ptt\":%s,\"client_ts_ms\":%lu}",
        static_cast<unsigned long>(seq),
        ptt_mode_ ? "ptt" : "auto",
        playback_active ? "agent_speaking" : "idle",
        mic_muted ? "true" : "false",
        ptt_held ? "true" : "false",
        static_cast<unsigned long>(client_ts_ms));
    if (written <= 0 || written >= static_cast<int>(sizeof(payload))) {
        return;
    }
    esp_err_t err = session_.PublishData(kClientAudioStateTopic, payload, state_changed);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Publish client audio state skipped: %s", esp_err_to_name(err));
        return;
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
    ptt_active_ = true;
    UpdateIdleAutoLeave();  // active: cancel the idle countdown
    // Hold-to-talk only applies in-room. Entering the room is an explicit tap (the
    // talk button is a "connect" button until connected) — a press while not
    // in-room is ignored rather than silently joining and dropping the first words.
    if (state_ != VoiceSessionState::InRoom) {
        ptt_active_ = false;
        ESP_LOGI(TAG, "PTT press ignored: not in room (tap to connect first)");
        return;
    }
    ESP_LOGI(TAG, "PTT press: mic open");
    PublishClientAudioState(AgentOutputActiveRecently());
}

void EidolonVoiceController::DoPttReleased()
{
    if (!ptt_mode_) {
        return;
    }
    ptt_active_ = false;
    // The ptt=false edge tells the server the user's turn is complete and closes
    // the capture gate.
    if (session_.IsConnected() && !control_room_) {
        ESP_LOGI(TAG, "PTT release: mic closed, turn committed");
        PublishClientAudioState(AgentOutputActiveRecently());
    } else {
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
    }
    if (on_agent_phase_) {
        on_agent_phase_(phase);
    }
}

}  // namespace eidolon
