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
constexpr TickType_t kAudioStatePublishInterval = pdMS_TO_TICKS(80);
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

TickType_t ReconnectDelayForAttempt(int attempt)
{
    uint32_t ms = kReconnectBaseDelayMs;
    for (int i = 0; i < attempt && ms < kReconnectMaxDelayMs; ++i) {
        ms <<= 1;
    }
    if (ms > kReconnectMaxDelayMs) {
        ms = kReconnectMaxDelayMs;
    }
    return pdMS_TO_TICKS(ms);
}
// Let the control-room ack flush before switching to the voice room.
constexpr TickType_t kRoomJoinSettleDelay = pdMS_TO_TICKS(500);
// Let the "succeeded" ack flush before reconnecting the control room.
constexpr TickType_t kActiveAckSettleDelay = pdMS_TO_TICKS(100);

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
}

namespace eidolon {

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
    if (on_state_changed_) {
        on_state_changed_(state);
    }
}

void EidolonVoiceController::OnLiveKitState(LiveKitConnectionState lk_state)
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
            // the reconnect task escalates it.
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

void EidolonVoiceController::ScheduleControlReconnect(const char* reason)
{
    if (control_room_ || switching_to_voice_ || control_reconnect_pending_ || !HasControlConfig()) {
        return;
    }

    control_reconnect_pending_ = true;
    ESP_LOGI(TAG, "Scheduling control room reconnect after %s (attempt %d)",
             reason ? reason : "disconnect", reconnect_attempts_);

    // Stack is sized for the rediscovery path (mDNS + HTTPS config fetch +
    // mbedtls signing), which runs inside this task once attempts pass the
    // threshold.
    BaseType_t created = xTaskCreate([](void* arg) {
        auto* self = static_cast<EidolonVoiceController*>(arg);
        int attempt = self->reconnect_attempts_;
        vTaskDelay(ReconnectDelayForAttempt(attempt));

        if (attempt >= kReconnectRediscoverAfter) {
            // The Hub address may have changed; re-query mDNS and re-fetch
            // config before reconnecting (best-effort, errors are logged).
            self->RediscoverHub();
        }
        if (attempt >= kReconnectUnreachableAfter) {
            self->SetState(VoiceSessionState::ServerUnreachable);
        }
        self->reconnect_attempts_ = attempt + 1;

        // Keep the guard up across ConnectControlRoom so its internal
        // synchronous Disconnect can't trigger a spurious re-schedule. Clear it
        // afterward so the (async) Failed callback drives the next attempt; if
        // the connect failed synchronously, drive it ourselves.
        esp_err_t err = self->ConnectControlRoom();
        self->control_reconnect_pending_ = false;
        if (err != ESP_OK) {
            self->ScheduleControlReconnect("control_retry");
        }
        vTaskDelete(NULL);
    }, "eidolon_ctrl_re", 8192, this, 3, nullptr);

    if (created != pdPASS) {
        control_reconnect_pending_ = false;
        ESP_LOGW(TAG, "Failed to create control reconnect task");
    }
}

void EidolonVoiceController::OnControlCommand(const std::string& payload)
{
    ControlCommand command = ParseControlCommand(payload);
    if (!command.valid) {
        ESP_LOGW(TAG, "Ignoring malformed control command");
        return;
    }
    if (command.expired) {
        ESP_LOGW(TAG, "Ignoring expired control command op=%s", command.op.c_str());
        session_.PublishData(
            kControlTopic,
            BuildControlAck(command, SystemInfo::GetMacAddress(), "expired", "COMMAND_EXPIRED"));
        return;
    }
    // Op dispatch registry. Add a row to support a new op; each handler runs on
    // a short-lived task via SpawnCommandTask. Defined as a static local so it
    // can take the address of private member handlers.
    struct ControlOpHandler {
        const char* op;
        const char* task_name;
        void (EidolonVoiceController::*handler)(const std::string&);
    };
    static const ControlOpHandler kControlOps[] = {
        {"config.refresh", "eidolon_ctrl", &EidolonVoiceController::HandleConfigRefreshCommand},
        {"room.join", "eidolon_join", &EidolonVoiceController::HandleRoomJoinCommand},
        {"playback.stop", "eidolon_playback", &EidolonVoiceController::HandlePlaybackStopCommand},
    };

    for (const auto& entry : kControlOps) {
        if (command.op == entry.op) {
            SpawnCommandTask(entry.task_name, command, entry.handler);
            return;
        }
    }

    ESP_LOGI(TAG, "Unsupported control command op=%s", command.op.c_str());
    session_.PublishData(
        kControlTopic,
        BuildControlAck(command, SystemInfo::GetMacAddress(), "unsupported", "UNSUPPORTED_OP"));
}

void EidolonVoiceController::SpawnCommandTask(
    const char* task_name, const ControlCommand& command,
    void (EidolonVoiceController::*handler)(const std::string&))
{
    session_.PublishData(kControlTopic,
                         BuildControlAck(command, SystemInfo::GetMacAddress(), "accepted", "OK"));

    struct CommandTaskArgs {
        EidolonVoiceController* self;
        std::string command_id;
        void (EidolonVoiceController::*handler)(const std::string&);
    };
    auto* args = new CommandTaskArgs{this, command.id, handler};
    BaseType_t created = xTaskCreate([](void* arg) {
        auto* a = static_cast<CommandTaskArgs*>(arg);
        (a->self->*a->handler)(a->command_id);
        delete a;
        vTaskDelete(NULL);
    }, task_name, 4096, args, 3, nullptr);
    if (created != pdPASS) {
        delete args;
        session_.PublishData(kControlTopic,
                             BuildControlAck(command, SystemInfo::GetMacAddress(), "failed",
                                             "TASK_CREATE_FAILED"));
    }
}

void EidolonVoiceController::OnSessionControl(const std::string& payload)
{
    if (payload.find("idle_timeout") == std::string::npos) {
        ESP_LOGI(TAG, "Ignoring unsupported session_control payload");
        return;
    }

    ESP_LOGI(TAG, "Session control -> idle timeout");
    BaseType_t created = xTaskCreate([](void* arg) {
        auto* self = static_cast<EidolonVoiceController*>(arg);
        self->HandleIdleTimeoutCommand();
        vTaskDelete(NULL);
    }, "eidolon_idle_ctl", 4096, this, 3, nullptr);

    if (created != pdPASS) {
        ESP_LOGW(TAG, "Failed to create idle-timeout handler task");
    }
}

void EidolonVoiceController::HandleConfigRefreshCommand(const std::string& command_id)
{
    ESP_LOGI(TAG, "Control command -> refresh Hub config");
    ControlCommand command;
    command.id = command_id;
    command.op = "config.refresh";

    if (RefreshHubConfig() != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered config refresh failed");
        session_.PublishData(kControlTopic,
                             BuildControlAck(command, SystemInfo::GetMacAddress(), "failed",
                                             "CONFIG_REFRESH_FAILED"));
        return;
    }

    if (config_.status == HubConfigStatus::Active) {
        session_.PublishData(kControlTopic,
                             BuildControlAck(command, SystemInfo::GetMacAddress(), "succeeded",
                                             "OK", "", "{\"status\":\"active\"}"));
        vTaskDelay(kActiveAckSettleDelay);
        ConnectControlRoom();
        return;
    }

    session_.PublishData(kControlTopic,
                         BuildControlAck(command, SystemInfo::GetMacAddress(), "succeeded", "OK"));
    ConnectControlRoom();
}

void EidolonVoiceController::HandleRoomJoinCommand(const std::string& command_id)
{
    ESP_LOGI(TAG, "Control command -> join voice room");
    ControlCommand command;
    command.id = command_id;
    command.op = "room.join";
    vTaskDelay(kRoomJoinSettleDelay);

    esp_err_t err = JoinRoom();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Control-triggered room join failed: %s", esp_err_to_name(err));
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
        ESP_LOGW(TAG, "Control-triggered playback stop failed: %s",
                 esp_err_to_name(flush_err));
        session_.PublishData(kControlTopic,
                             BuildControlAck(command, SystemInfo::GetMacAddress(), "failed",
                                             "PLAYBACK_FLUSH_FAILED",
                                             esp_err_to_name(flush_err)));
        return;
    }

    HandleAgentPhase(AgentPhase::Silent);
    PublishClientAudioState(false);
    session_.PublishData(kControlTopic,
                         BuildControlAck(command, SystemInfo::GetMacAddress(), "completed", "OK"));
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

void EidolonVoiceController::OnHubActivationSucceeded()
{
    session_.SetOnStateChanged([this](LiveKitConnectionState s) { OnLiveKitState(s); });
    session_.SetOnControlCommand([this](const std::string& payload) {
        OnControlCommand(payload);
    });
    session_.SetOnSessionControl([this](const std::string& payload) {
        OnSessionControl(payload);
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

#if CONFIG_EIDOLON_AUTO_JOIN_ON_ACTIVATION
    if (HasActiveConfig()) {
        JoinRoom();
    } else {
        ConnectControlRoom();
    }
#else
    if (HasControlConfig()) {
        ConnectControlRoom();
    }
#endif
}

void EidolonVoiceController::OnNetworkLost()
{
    StopAudioStatePublisher();
    control_room_ = false;
    reconnect_attempts_ = 0;  // distinct cause; reconnect starts fresh on restore
    session_.Disconnect(true);
    SetState(StateForConfig(config_));
}

esp_err_t EidolonVoiceController::JoinRoom()
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
            ESP_LOGW(TAG, "Join blocked: config refresh failed: %s",
                     esp_err_to_name(refresh_err));
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

esp_err_t EidolonVoiceController::LeaveRoom()
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

void EidolonVoiceController::StartAudioStatePublisher()
{
    if (audio_state_task_running_) {
        return;
    }
    audio_state_task_stop_ = false;
    audio_state_seq_ = 0;
    audio_state_sent_ = false;
    audio_state_task_running_ = true;
    BaseType_t created = xTaskCreate([](void* arg) {
        auto* self = static_cast<EidolonVoiceController*>(arg);
        self->AudioStatePublisherTask();
        self->audio_state_task_running_ = false;
        vTaskDelete(nullptr);
    }, "eidolon_audio_state", 4096, this, 3, nullptr);
    if (created != pdPASS) {
        audio_state_task_running_ = false;
        ESP_LOGW(TAG, "Failed to create audio state publisher task");
    }
}

void EidolonVoiceController::StopAudioStatePublisher()
{
    audio_state_task_stop_ = true;
}

void EidolonVoiceController::AudioStatePublisherTask()
{
    while (!audio_state_task_stop_ && !control_room_ && session_.IsConnected()) {
        PublishClientAudioState(AgentOutputActiveRecently());
        vTaskDelay(kAudioStatePublishInterval);
    }

    if (session_.IsConnected() && !control_room_) {
        PublishClientAudioState(false);
    }
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

esp_err_t EidolonVoiceController::SetMicEnabled(bool enabled)
{
    mic_enabled_ = enabled;
    if (session_.IsConnected() && !control_room_) {
        PublishClientAudioState(AgentOutputActiveRecently());
    } else {
        eidolon_livekit_board_set_capture_enabled(enabled);
    }
    ESP_LOGI(TAG, "Mic enabled=%d", enabled ? 1 : 0);
    return ESP_OK;
}

void EidolonVoiceController::OnPttPressed()
{
    if (!ptt_mode_) {
        return;
    }
    ptt_active_ = true;
    // First press brings up the room. Capture opens once connected, when the
    // audio-state publisher runs and gates the mic on ptt_active_ (still true).
    if (state_ != VoiceSessionState::InRoom) {
        ESP_LOGI(TAG, "PTT press: joining room");
        JoinRoom();
        return;
    }
    ESP_LOGI(TAG, "PTT press: mic open");
    PublishClientAudioState(AgentOutputActiveRecently());
}

void EidolonVoiceController::OnPttReleased()
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
}

void EidolonVoiceController::SetOnTranscription(std::function<void(const TranscriptionEvent&)> cb)
{
    session_.SetOnTranscription(std::move(cb));
}

void EidolonVoiceController::SetOnAgentPhase(std::function<void(AgentPhase)> cb)
{
    on_agent_phase_ = std::move(cb);
    session_.SetOnAgentPhase([this](AgentPhase phase) { HandleAgentPhase(phase); });
}

void EidolonVoiceController::HandleAgentPhase(AgentPhase phase)
{
    bool changed = phase != agent_phase_;
    agent_phase_ = phase;
    if (changed) {
        ESP_LOGI(TAG, "Agent phase -> %s", AgentPhaseName(phase));
        if (session_.IsConnected() && !control_room_) {
            PublishClientAudioState(AgentOutputActiveRecently());
        }
    }
    if (on_agent_phase_) {
        on_agent_phase_(phase);
    }
}

}  // namespace eidolon
