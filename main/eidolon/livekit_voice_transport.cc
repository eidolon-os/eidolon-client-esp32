#include "livekit_voice_transport.h"

#include <esp_log.h>
#include <livekit.h>

#define TAG "LiveKitVoiceTransport"

namespace eidolon {

LiveKitVoiceTransport::LiveKitVoiceTransport(VoiceSessionCallbacks cb)
{
    if (livekit_system_init() != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_system_init failed");
    }

    mic_enabled_ = device_store_.LoadMicEnabled(true);
    controller_ = std::make_unique<EidolonVoiceController>();

    if (cb.on_session_state) {
        controller_->SetOnStateChanged(std::move(cb.on_session_state));
    }
    if (cb.on_transcription) {
        controller_->SetOnTranscription(std::move(cb.on_transcription));
    }
    if (cb.on_agent_phase) {
        controller_->SetOnAgentPhase(std::move(cb.on_agent_phase));
    }
    controller_->SetMicEnabled(mic_enabled_);
}

LiveKitVoiceTransport::~LiveKitVoiceTransport() = default;

void LiveKitVoiceTransport::OnActivationComplete()
{
    controller_->OnHubActivationSucceeded();
}

void LiveKitVoiceTransport::OnNetworkLost()
{
    controller_->OnNetworkLost();
}

void LiveKitVoiceTransport::JoinSession()
{
    controller_->JoinRoom();
}

void LiveKitVoiceTransport::LeaveSession()
{
    controller_->LeaveRoom();
}

void LiveKitVoiceTransport::ToggleSession()
{
    auto state = controller_->GetState();
    switch (state) {
    case VoiceSessionState::PendingApproval:
    case VoiceSessionState::WaitingBinding:
    case VoiceSessionState::Unauthorized:
        ESP_LOGI(TAG, "Pairing state may be stale, refreshing before voice join");
        JoinSession();
        break;
    case VoiceSessionState::ConfigReady:
    case VoiceSessionState::Idle:
    case VoiceSessionState::Error:
    case VoiceSessionState::ServerUnreachable:
        JoinSession();
        break;
    case VoiceSessionState::Connecting:
    case VoiceSessionState::Reconnecting:
        LeaveSession();
        break;
    case VoiceSessionState::InRoom:
        ESP_LOGI(TAG, "Voice room already active; touch is passive in product mode");
        break;
    }
}

bool LiveKitVoiceTransport::IsInSession() const
{
    return controller_->GetState() == VoiceSessionState::InRoom;
}

void LiveKitVoiceTransport::SetMicrophoneEnabled(bool enabled)
{
    mic_enabled_ = enabled;
    device_store_.SaveMicEnabled(enabled);
    controller_->SetMicEnabled(enabled);
}

bool LiveKitVoiceTransport::IsMicrophoneEnabled() const
{
    return mic_enabled_;
}

VoiceSessionState LiveKitVoiceTransport::GetSessionState() const
{
    return controller_->GetState();
}

std::unique_ptr<IVoiceSessionTransport> CreateLiveKitVoiceTransport(VoiceSessionCallbacks cb)
{
    return std::make_unique<LiveKitVoiceTransport>(std::move(cb));
}

}  // namespace eidolon
