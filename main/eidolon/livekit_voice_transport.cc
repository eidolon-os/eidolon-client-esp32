#include "livekit_voice_transport.h"

#include <esp_log.h>
#include <livekit.h>

#define TAG "LiveKitVoiceTransport"

namespace eidolon {

LiveKitVoiceTransport::LiveKitVoiceTransport(VoiceSessionCallbacks cb, GuardService* guard_service)
{
    if (livekit_system_init() != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "livekit_system_init failed");
    }

    mic_enabled_ = device_store_.LoadMicEnabled(true);
    controller_ = std::make_unique<EidolonVoiceController>(guard_service);
    on_runtime_status_ = std::move(cb.on_runtime_status);

    auto on_session_state = std::move(cb.on_session_state);
    controller_->SetOnStateChanged(
        [this, on_session_state = std::move(on_session_state)](VoiceSessionState state) {
            if (on_session_state) {
                on_session_state(state);
            }
            NotifyRuntimeStatus(state);
        });
    if (cb.on_operational_ready) {
        controller_->SetOnOperationalReady(std::move(cb.on_operational_ready));
    }
    if (cb.on_transcription) {
        controller_->SetOnTranscription(std::move(cb.on_transcription));
    }
    if (cb.on_agent_phase) {
        controller_->SetOnAgentPhase(std::move(cb.on_agent_phase));
    }
    if (cb.on_presence_wake_phase) {
        controller_->SetOnPresenceWakePhase(std::move(cb.on_presence_wake_phase));
    }
    if (cb.on_ptt_turn_status) {
        controller_->SetOnPttTurnStatus(std::move(cb.on_ptt_turn_status));
    }
    controller_->SetMicEnabled(mic_enabled_);
}

VoiceRuntimeStatus LiveKitVoiceTransport::BuildRuntimeStatus(VoiceSessionState state) const
{
    VoiceRuntimeStatus status;
    status.mic_enabled = mic_enabled_;
    status.end_reason = controller_->LastEndReason();
    switch (controller_->GetConfigStatus()) {
    case HubConfigStatus::PendingApproval:
        status.enrollment = EnrollmentPhase::PendingReview;
        break;
    case HubConfigStatus::WaitingBinding:
    case HubConfigStatus::Active:
        status.enrollment = EnrollmentPhase::ClaimActive;
        break;
    case HubConfigStatus::Revoked:
        status.enrollment = EnrollmentPhase::Revoked;
        break;
    }

    switch (state) {
    case VoiceSessionState::Idle:
        status.enrollment = EnrollmentPhase::Unknown;
        status.service = ServicePhase::Unavailable;
        break;
    case VoiceSessionState::PendingApproval:
        status.enrollment = EnrollmentPhase::PendingReview;
        status.service = ServicePhase::Unavailable;
        break;
    case VoiceSessionState::WaitingBinding:
        status.service = ServicePhase::Preparing;
        break;
    case VoiceSessionState::ConfigReady:
        status.service = ServicePhase::Ready;
        status.conversation = status.end_reason == EndReason::None
                                  ? ConversationPhase::Closed
                                  : ConversationPhase::Ended;
        break;
    case VoiceSessionState::Connecting:
    case VoiceSessionState::Opening:
        status.service = ServicePhase::Connecting;
        status.conversation = ConversationPhase::Opening;
        break;
    case VoiceSessionState::InRoom:
        status.service = ServicePhase::Ready;
        status.conversation = ConversationPhase::Active;
        break;
    case VoiceSessionState::Reconnecting:
        status.service = ServicePhase::Reconnecting;
        status.conversation = ConversationPhase::Reconnecting;
        break;
    case VoiceSessionState::ServerUnreachable:
        status.service = ServicePhase::Unreachable;
        status.conversation = ConversationPhase::Failed;
        break;
    case VoiceSessionState::Unauthorized:
        status.enrollment = EnrollmentPhase::Revoked;
        status.service = ServicePhase::Unavailable;
        break;
    case VoiceSessionState::Error:
    default:
        status.service = ServicePhase::Unreachable;
        status.conversation = ConversationPhase::Failed;
        break;
    }
    return status;
}

void LiveKitVoiceTransport::NotifyRuntimeStatus(VoiceSessionState state)
{
    if (on_runtime_status_) {
        on_runtime_status_(BuildRuntimeStatus(state));
    }
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

void LiveKitVoiceTransport::OnNetworkRestored()
{
    controller_->OnNetworkRestored();
}

void LiveKitVoiceTransport::QuiesceForCommissioning(
    std::function<void(bool)> completion)
{
    controller_->QuiesceForCommissioning(std::move(completion));
}

void LiveKitVoiceTransport::OnAmbientPresenceChanged(bool present)
{
    controller_->OnAmbientPresenceChanged(present);
}

void LiveKitVoiceTransport::JoinSession()
{
    ESP_LOGI(TAG, "[voice_request] JoinSession state=%s",
             EidolonVoiceController::VoiceStateName(controller_->GetState()));
    controller_->JoinRoom();
}

void LiveKitVoiceTransport::LeaveSession()
{
    ESP_LOGI(TAG, "[voice_request] LeaveSession state=%s",
             EidolonVoiceController::VoiceStateName(controller_->GetState()));
    controller_->LeaveRoom();
}

void LiveKitVoiceTransport::PttPress()
{
    ESP_LOGI(TAG, "[voice_request] PttPress state=%s",
             EidolonVoiceController::VoiceStateName(controller_->GetState()));
    controller_->OnPttPressed();
}

void LiveKitVoiceTransport::PttRelease()
{
    ESP_LOGI(TAG, "[voice_request] PttRelease state=%s",
             EidolonVoiceController::VoiceStateName(controller_->GetState()));
    controller_->OnPttReleased();
}

void LiveKitVoiceTransport::ToggleSession()
{
    auto state = controller_->GetState();
    ESP_LOGI(TAG, "[voice_request] ToggleSession state=%s",
             EidolonVoiceController::VoiceStateName(state));
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
    case VoiceSessionState::Opening:
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
    NotifyRuntimeStatus(controller_->GetState());
}

bool LiveKitVoiceTransport::IsMicrophoneEnabled() const
{
    return mic_enabled_;
}

VoiceSessionState LiveKitVoiceTransport::GetSessionState() const
{
    return controller_->GetState();
}

EndReason LiveKitVoiceTransport::LastEndReason() const
{
    return controller_->LastEndReason();
}

std::unique_ptr<IVoiceSessionTransport> CreateLiveKitVoiceTransport(
    VoiceSessionCallbacks cb, GuardService* guard_service)
{
    return std::make_unique<LiveKitVoiceTransport>(std::move(cb), guard_service);
}

}  // namespace eidolon
