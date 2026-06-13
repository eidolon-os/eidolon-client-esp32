#include "eidolon_voice_controller.h"

#include "board.h"
#include "control_protocol.h"
#include "hub_config_client.h"
#include "hub_config_store.h"
#include "livekit_board.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>

#define TAG "EidolonVoice"

namespace {
constexpr const char* kClientAudioStateTopic = "client.audio_state";
constexpr int64_t kPlaybackActiveWindowUs = 800 * 1000;
constexpr TickType_t kAudioStatePublishInterval = pdMS_TO_TICKS(500);
// Delay before a control-room reconnect attempt after a disconnect.
constexpr TickType_t kControlReconnectDelay = pdMS_TO_TICKS(1000);
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
        return VoiceSessionState::Error;
    }
    return VoiceSessionState::Error;
}

bool EidolonVoiceController::HasActiveConfig() const
{
    return config_.status == HubConfigStatus::Active && !config_.server_url.empty() &&
           !config_.token.empty();
}

bool EidolonVoiceController::HasControlConfig() const
{
    if (config_.status != HubConfigStatus::Active) {
        return !config_.server_url.empty() && !config_.token.empty();
    }
    return !config_.control_server_url.empty() && !config_.control_token.empty() &&
           !config_.control_room_name.empty();
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
        if (lk_state == LiveKitConnectionState::Failed) {
            ESP_LOGW(TAG, "Control room connection failed");
        }
        if (!switching_to_voice_) {
            SetState(StateForConfig(config_));
        }
        return;
    }

    switch (lk_state) {
    case LiveKitConnectionState::Connecting:
        SetState(VoiceSessionState::Connecting);
        break;
    case LiveKitConnectionState::Connected:
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
    ESP_LOGI(TAG, "Scheduling control room reconnect after %s", reason ? reason : "disconnect");

    BaseType_t created = xTaskCreate([](void* arg) {
        auto* self = static_cast<EidolonVoiceController*>(arg);
        vTaskDelay(kControlReconnectDelay);
        self->ConnectControlRoom();
        self->control_reconnect_pending_ = false;
        vTaskDelete(NULL);
    }, "eidolon_ctrl_re", 4096, this, 3, nullptr);

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

    if (config_.server_url.empty() || config_.token.empty()) {
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

    Esp32HubConfig control_config = config_;
    if (config_.status == HubConfigStatus::Active) {
        control_config.server_url = config_.control_server_url;
        control_config.token = config_.control_token;
        control_config.identity = config_.control_identity.empty() ? config_.identity : config_.control_identity;
        control_config.room_name = config_.control_room_name;
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
    bool mic_muted = !mic_enabled_;
    eidolon_livekit_board_set_capture_enabled(mic_enabled_);
    bool state_changed = !audio_state_sent_ ||
                         playback_active != last_audio_playback_active_ ||
                         mic_muted != last_audio_mic_muted_;
    uint32_t seq = ++audio_state_seq_;
    int written = snprintf(
        payload,
        sizeof(payload),
        "{\"type\":\"client.audio_state\",\"seq\":%lu,\"input_mode\":\"auto\","
        "\"playback_state\":\"%s\",\"mic_muted\":%s,"
        "\"manual_interrupt\":false,\"ptt\":false,\"client_ts_ms\":%lu}",
        static_cast<unsigned long>(seq),
        playback_active ? "agent_speaking" : "idle",
        mic_muted ? "true" : "false",
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
