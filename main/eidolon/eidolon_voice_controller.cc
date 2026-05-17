#include "eidolon_voice_controller.h"

#include "board.h"
#include "hub_config_client.h"
#include "hub_config_store.h"
#include "system_info.h"

#include <esp_log.h>

#define TAG "EidolonVoice"

namespace eidolon {

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
    switch (lk_state) {
    case LiveKitConnectionState::Connecting:
        SetState(VoiceSessionState::Connecting);
        break;
    case LiveKitConnectionState::Connected:
        SetState(VoiceSessionState::InRoom);
        break;
    case LiveKitConnectionState::Reconnecting:
        SetState(VoiceSessionState::Reconnecting);
        break;
    case LiveKitConnectionState::Failed:
        SetState(VoiceSessionState::Error);
        break;
    case LiveKitConnectionState::Disconnected:
        if (state_ != VoiceSessionState::Idle && state_ != VoiceSessionState::ConfigReady) {
            SetState(VoiceSessionState::ConfigReady);
        }
        break;
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
    SetState(VoiceSessionState::ConfigReady);
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
    return ESP_OK;
}

void EidolonVoiceController::OnHubActivationSucceeded()
{
    session_.SetOnStateChanged([this](LiveKitConnectionState s) { OnLiveKitState(s); });

    if (LoadStoredConfig() != ESP_OK) {
        return;
    }

#if CONFIG_EIDOLON_JOIN_ROOM_ON_HUB_READY
    JoinRoom();
#endif
}

void EidolonVoiceController::OnNetworkLost()
{
    LeaveRoom();
    SetState(VoiceSessionState::ConfigReady);
}

esp_err_t EidolonVoiceController::JoinRoom()
{
    if (config_.server_url.empty() || config_.token.empty()) {
        if (LoadStoredConfig() != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    SetState(VoiceSessionState::Connecting);
    esp_err_t err = session_.Connect(config_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connect failed, refreshing Hub token");
        if (RefreshHubConfig() == ESP_OK) {
            err = session_.Connect(config_);
        }
    }
    if (err != ESP_OK) {
        SetState(VoiceSessionState::Error);
    }
    return err;
}

esp_err_t EidolonVoiceController::LeaveRoom()
{
    esp_err_t err = session_.Disconnect();
    if (state_ != VoiceSessionState::Idle) {
        SetState(VoiceSessionState::ConfigReady);
    }
    return err;
}

esp_err_t EidolonVoiceController::SetMicEnabled(bool enabled)
{
    mic_enabled_ = enabled;
    // LiveKit ESP32 SDK v0.3.7: no dedicated mute API; future: capturer pause.
    ESP_LOGI(TAG, "Mic enabled=%d (publish mute deferred)", enabled ? 1 : 0);
    return ESP_OK;
}

void EidolonVoiceController::SetOnTranscription(std::function<void(const std::string&)> cb)
{
    session_.SetOnTranscription(std::move(cb));
}

}  // namespace eidolon
