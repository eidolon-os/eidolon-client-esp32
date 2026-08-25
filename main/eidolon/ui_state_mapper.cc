#include "ui_state_mapper.h"

#include "eidolon_ui_labels.h"

namespace eidolon {

namespace {

UiScene RuntimeScene(RuntimePhase phase)
{
    switch (phase) {
    case RuntimePhase::Booting:
        return UiScene::Starting;
    case RuntimePhase::LoadingAssets:
        return UiScene::Loading;
    case RuntimePhase::NetworkScanning:
    case RuntimePhase::NetworkConnecting:
        return UiScene::Network;
    case RuntimePhase::Commissioning:
        return UiScene::Commissioning;
    case RuntimePhase::Updating:
        return UiScene::Updating;
    case RuntimePhase::RecoveryRequired:
        return UiScene::RecoveryRequired;
    case RuntimePhase::Fault:
        return UiScene::Error;
    case RuntimePhase::Normal:
    default:
        return UiScene::Ready;
    }
}

UiScene ConversationScene(const EidolonRuntimeStatus& status)
{
    switch (status.conversation) {
    case ConversationPhase::Opening:
        return UiScene::OpeningConversation;
    case ConversationPhase::Active:
        return UiScene::Conversation;
    case ConversationPhase::Reconnecting:
        return UiScene::Reconnecting;
    case ConversationPhase::Ended:
        return UiScene::Ended;
    case ConversationPhase::Failed:
        return UiScene::Error;
    case ConversationPhase::Closed:
    default:
        break;
    }

    switch (status.service) {
    case ServicePhase::Reconnecting:
        return UiScene::Reconnecting;
    case ServicePhase::Connecting:
    case ServicePhase::DiscoveringAuthority:
    case ServicePhase::Registering:
    case ServicePhase::Preparing:
    case ServicePhase::Unavailable:
        return UiScene::PreparingService;
    case ServicePhase::Unreachable:
        return UiScene::Error;
    case ServicePhase::Ready:
    default:
        return UiScene::Ready;
    }
}

UiScene SelectScene(const EidolonRuntimeStatus& status)
{
    if (status.enrollment == EnrollmentPhase::Revoked) {
        return UiScene::Removed;
    }
    if (status.runtime != RuntimePhase::Normal) {
        return RuntimeScene(status.runtime);
    }

    switch (status.enrollment) {
    case EnrollmentPhase::PendingReview:
        return UiScene::WaitingApproval;
    case EnrollmentPhase::Unknown:
        return UiScene::PreparingService;
    case EnrollmentPhase::ClaimActive:
    case EnrollmentPhase::Revoked:
    default:
        break;
    }

    return ConversationScene(status);
}

void ResolveConversationPresentation(const EidolonRuntimeStatus& status,
                                     EidolonUiModel& model)
{
    if (model.scene != UiScene::Conversation) {
        return;
    }
    switch (status.turn) {
    case TurnPhase::Recording:
    case TurnPhase::UserSpeaking:
        model.state_label = "REC";
        model.status_text = "Listening";
        model.detail_text = "Release to send";
        model.emotion = "happy";
        break;
    case TurnPhase::Committing:
        model.state_label = "SEND";
        model.status_text = "Sending";
        model.detail_text = "Waiting for response";
        model.emotion = "thinking";
        break;
    case TurnPhase::AgentThinking:
        model.state_label = "THINK";
        model.status_text = "Thinking";
        model.detail_text = "Working on it...";
        model.emotion = "thinking";
        break;
    case TurnPhase::AgentSpeaking:
        model.state_label = "SPEAK";
        model.status_text = "Speaking";
        model.detail_text = "Eidolon is speaking";
        model.emotion = "happy";
        break;
    case TurnPhase::Idle:
    default:
        model.state_label = status.interaction_mode == InteractionMode::PushToTalk
                                ? "TALK"
                                : "LISTEN";
        model.status_text = status.interaction_mode == InteractionMode::PushToTalk
                                ? "Ready to talk"
                                : "Listening";
        model.detail_text = status.interaction_mode == InteractionMode::PushToTalk
                                ? "Hold to talk"
                                : "Listening...";
        break;
    }
}

void ResolveActions(const EidolonRuntimeStatus& status, EidolonUiModel& model)
{
    switch (model.scene) {
    case UiScene::Ready:
    case UiScene::Ended:
        model.primary_intent = UiIntent::OpenConversation;
        model.primary_label = "JOIN";
        model.primary_enabled = true;
        break;
    case UiScene::OpeningConversation:
    case UiScene::Reconnecting:
        model.primary_label = "...";
        model.show_end_action = true;
        break;
    case UiScene::Conversation:
        model.show_end_action = true;
        model.primary_enabled = true;
        if (status.interaction_mode == InteractionMode::PushToTalk) {
            model.primary_intent = status.turn == TurnPhase::Recording
                                       ? UiIntent::CommitTalk
                                       : UiIntent::BeginTalk;
            model.primary_label = status.turn == TurnPhase::Recording ? "REC" : "TALK";
        } else {
            model.primary_intent = UiIntent::ToggleMicrophone;
            model.primary_label = status.mic_enabled ? "MIC" : "MUTE";
        }
        break;
    default:
        break;
    }
}

const std::string* DetailOverride(const EidolonRuntimeStatus& status,
                                  UiScene scene)
{
    if (status.runtime != RuntimePhase::Normal) {
        return &status.runtime_detail;
    }
    if (scene == UiScene::WaitingApproval || scene == UiScene::Removed) {
        return &status.enrollment_detail;
    }
    if (status.conversation == ConversationPhase::Closed ||
        status.conversation == ConversationPhase::Failed) {
        return &status.service_detail;
    }
    return nullptr;
}

}  // namespace

EidolonUiModel UiStateProjector::Project(const EidolonRuntimeStatus& status)
{
    EidolonUiModel model;
    model.scene = SelectScene(status);
    model.interaction_mode = status.interaction_mode;
    model.turn = status.turn;
    model.end_reason = status.end_reason;
    model.show_mute_icon = !status.mic_enabled;
    model.state_label = UiSceneLabel(model.scene);
    model.status_text = UiSceneStatus(model.scene);
    const std::string* detail = DetailOverride(status, model.scene);
    model.detail_text = detail == nullptr || detail->empty()
                            ? UiSceneDetail(model.scene, status.end_reason)
                            : detail->c_str();
    model.emotion = UiSceneEmotion(model.scene);

    if (!status.last_transcription.empty() && model.scene == UiScene::Conversation) {
        model.subtitle = status.last_transcription.c_str();
        model.subtitle_role = status.last_transcription_role;
    }

    if (status.presence_wake == PresenceWakePhase::VerifyingOwner &&
        model.scene == UiScene::Ready) {
        model.state_label = "VERIFY";
        model.status_text = "Verifying owner";
        model.detail_text = "Checking owner...";
    } else if (status.presence_wake == PresenceWakePhase::OwnerRecognized &&
               model.scene == UiScene::Ready) {
        model.state_label = "OWNER";
        model.status_text = "Owner recognized";
        model.detail_text = "Opening conversation";
        model.emotion = "happy";
    }

    ResolveConversationPresentation(status, model);
    ResolveActions(status, model);
    model.severity = model.scene == UiScene::Error ||
                             model.scene == UiScene::RecoveryRequired ||
                             model.scene == UiScene::Removed ||
                             (model.scene == UiScene::Ended &&
                              model.end_reason == EndReason::Error)
                         ? UiSeverity::Error
                         : (model.scene == UiScene::WaitingApproval ||
                                    model.scene == UiScene::PreparingService
                                ? UiSeverity::Attention
                                : UiSeverity::Normal);
    return model;
}

bool UiStateProjector::AllowsIntent(const EidolonRuntimeStatus& status, UiIntent intent)
{
    const auto model = Project(status);
    switch (intent) {
    case UiIntent::OpenConversation:
        return model.primary_enabled && model.primary_intent == intent;
    case UiIntent::CloseConversation:
        return model.show_end_action;
    case UiIntent::BeginTalk:
        return model.scene == UiScene::Conversation &&
               status.interaction_mode == InteractionMode::PushToTalk &&
               status.turn != TurnPhase::Recording;
    case UiIntent::CommitTalk:
        return model.scene == UiScene::Conversation &&
               status.interaction_mode == InteractionMode::PushToTalk &&
               status.turn == TurnPhase::Recording;
    case UiIntent::ToggleMicrophone:
        return model.scene == UiScene::Conversation &&
               status.interaction_mode == InteractionMode::Streaming;
    case UiIntent::None:
    default:
        return false;
    }
}

}  // namespace eidolon
