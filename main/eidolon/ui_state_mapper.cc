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
        // An end summary is actionable only while the service can open the next
        // conversation. Recovery takes precedence when the operational channel
        // is unavailable.
        if (status.service == ServicePhase::Ready) {
            return UiScene::Ended;
        }
        break;
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
        return UiScene::Reconnecting;
    case ServicePhase::Fault:
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
        if (status.service == ServicePhase::Fault) return UiScene::Error;
        if (status.service == ServicePhase::Unreachable || status.service == ServicePhase::Reconnecting)
            return UiScene::Reconnecting;
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
    switch (model.turn) {
    case TurnPhase::Recording:
        model.state_label = "REC";
        model.status_text = "Listening";
        model.detail_text = "Release to send";
        model.emotion = "happy";
        break;
    case TurnPhase::UserSpeaking:
        model.state_label = "LISTEN";
        model.status_text = "Listening";
        model.detail_text = "Listening...";
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
        model.state_label = IsPushToTalk(status.interaction_mode)
                                ? "TALK"
                                : "LISTEN";
        model.status_text = IsPushToTalk(status.interaction_mode)
                                ? "Ready to talk"
                                : "Listening";
        model.detail_text = IsPushToTalk(status.interaction_mode)
                                ? "Hold to talk"
                                : "Listening...";
        break;
    }
}

void ResolveServicePresentation(const EidolonRuntimeStatus& status, EidolonUiModel& model)
{
    if (model.scene != UiScene::PreparingService) return;
    switch (status.service) {
    case ServicePhase::DiscoveringAuthority:
        model.status_text = "Finding service";
        model.detail_text = "Looking for your Hub...";
        break;
    case ServicePhase::Registering:
        model.status_text = "Checking device";
        model.detail_text = "Confirming device access...";
        break;
    case ServicePhase::Connecting:
        model.status_text = "Connecting";
        model.detail_text = "Connecting to your service...";
        break;
    case ServicePhase::Preparing:
        model.status_text = "Waiting for service";
        model.detail_text = "Waiting for service configuration";
        break;
    default:
        model.detail_text = status.enrollment == EnrollmentPhase::Unknown
            ? "Checking device status..." : "Preparing your service...";
        break;
    }
    if (!status.service_detail.empty()) model.detail_text = status.service_detail.c_str();
}

void ResolveRecoveryPresentation(const EidolonRuntimeStatus& status,
                                 EidolonUiModel& model)
{
    if (model.scene != UiScene::Reconnecting ||
        status.conversation == ConversationPhase::Reconnecting) {
        return;
    }
    model.state_label = "RETRY";
    model.status_text = status.service == ServicePhase::Unreachable
        ? "Host unavailable" : "Reconnecting to Host";
    model.detail_text = status.service == ServicePhase::Unreachable
        ? "Check Host and Wi-Fi. Retrying automatically."
        : "Please wait. Reconnecting automatically.";
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
        model.primary_label = "...";
        model.end_allowed = true;
        break;
    case UiScene::Reconnecting:
        model.primary_label = "...";
        model.end_allowed = status.conversation == ConversationPhase::Reconnecting;
        break;
    case UiScene::Conversation:
        model.end_allowed = true;
        model.primary_enabled = true;
        if (IsPushToTalk(status.interaction_mode)) {
            model.primary_intent = model.turn == TurnPhase::Recording
                                       ? UiIntent::CommitTalk
                                       : UiIntent::BeginTalk;
            model.primary_label = model.turn == TurnPhase::Recording ? "REC" : "TALK";
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

EidolonUiModel UiStateProjector::Project(const EidolonRuntimeStatus& status,
                                        const UiInputProfile& inputs)
{
    EidolonUiModel model;
    model.scene = SelectScene(status);
    model.interaction_mode = status.interaction_mode;
    model.mode_label = InteractionModeLabel(status.interaction_mode);
    model.turn = NormalizeTurnPhase(status.interaction_mode, status.turn);
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

    ResolveServicePresentation(status, model);
    ResolveConversationPresentation(status, model);
    ResolveRecoveryPresentation(status, model);
    ResolveActions(status, model);
    model.severity = model.scene == UiScene::Error ||
                             model.scene == UiScene::RecoveryRequired ||
                             model.scene == UiScene::Removed ||
                             (model.scene == UiScene::Ended &&
                              model.end_reason == EndReason::Error)
                         ? UiSeverity::Error
                         : (model.scene == UiScene::WaitingApproval ||
                                    model.scene == UiScene::PreparingService ||
                                    model.scene == UiScene::Reconnecting
                                ? UiSeverity::Attention
                                : UiSeverity::Normal);
    model.touch_navigation = inputs.Has(UiInputSource::Touch);
    model.show_setup_action = inputs.Binding(UiIntent::OpenSetup,UiInputSource::Touch) &&
        status.runtime!=RuntimePhase::Updating;
    model.show_end_action = model.end_allowed &&
        inputs.Binding(UiIntent::CloseConversation, UiInputSource::Touch);
    if (model.primary_enabled && inputs.Binding(model.primary_intent, UiInputSource::Touch)) {
        model.primary_presentation = UiActionPresentation::TouchControl;
    } else {
        const UiInputBinding* hint = nullptr;
        for (auto candidate : {model.primary_intent, UiIntent::CloseConversation}) {
            if (candidate==UiIntent::None ||
                (candidate==UiIntent::CloseConversation && !model.end_allowed)) continue;
            for (size_t i=0; i<inputs.binding_count; ++i) {
                const auto& binding=inputs.bindings[i];
                if (binding.source!=UiInputSource::Touch && inputs.Has(binding.source) &&
                    binding.intent==candidate) { hint=&binding;break; }
            }
            if (hint) break;
        }
        if (hint) {
            model.primary_presentation = UiActionPresentation::InputHint;
            model.input_hint = hint->hint;
        }
    }
    if (model.primary_enabled && model.primary_intent==UiIntent::OpenConversation &&
        model.primary_presentation==UiActionPresentation::Hidden) {
        model.detail_text = inputs.automatic_start ? "Waiting for automatic connection" : "No start input available";
    }
    if (model.scene==UiScene::Conversation && IsPushToTalk(model.interaction_mode) &&
        model.turn==TurnPhase::Idle &&
        !inputs.Binding(UiIntent::BeginTalk,UiInputSource::Touch) &&
        !inputs.Binding(UiIntent::BeginTalk,UiInputSource::TalkButton)) {
        model.detail_text = "Talk input unavailable";
    }
    return model;
}

bool UiStateProjector::AllowsIntent(const EidolonRuntimeStatus& status, UiIntent intent)
{
    const auto model = Project(status);
    switch (intent) {
    case UiIntent::OpenConversation:
        return model.primary_enabled && model.primary_intent == intent;
    case UiIntent::CloseConversation:
        return model.end_allowed;
    case UiIntent::BeginTalk:
        return model.scene == UiScene::Conversation &&
               IsPushToTalk(model.interaction_mode) &&
               model.turn != TurnPhase::Recording;
    case UiIntent::CommitTalk:
        return model.scene == UiScene::Conversation &&
               IsPushToTalk(model.interaction_mode) &&
               model.turn == TurnPhase::Recording;
    case UiIntent::ToggleMicrophone:
        return model.scene == UiScene::Conversation &&
               IsAutomaticEndpointing(model.interaction_mode);
    case UiIntent::OpenSetup:
        return status.runtime!=RuntimePhase::Updating;
    case UiIntent::None:
    default:
        return false;
    }
}

UiIntent UiStateProjector::ResolveInput(const EidolonRuntimeStatus& status,
                                       const UiInputProfile& inputs, UiInputSource source,
                                       UiInputGesture gesture)
{
    if (!inputs.Has(source)) return UiIntent::None;
    for (size_t i=0; i<inputs.binding_count; ++i) {
        const auto& binding=inputs.bindings[i];
        if (binding.source==source && binding.gesture==gesture &&
            inputs.Binding(binding.intent,source) && AllowsIntent(status,binding.intent)) return binding.intent;
    }
    return UiIntent::None;
}

bool UiStateProjector::IsConversationState(DeviceState state)
{
    return state == kDeviceStateConnecting || state == kDeviceStateListening ||
           state == kDeviceStateSpeaking;
}

DeviceState UiStateProjector::ProjectLegacyDeviceState(
    const EidolonRuntimeStatus& status, DeviceState current)
{
    if (status.conversation == ConversationPhase::Opening ||
        status.conversation == ConversationPhase::Failed ||
        status.service == ServicePhase::Reconnecting ||
        status.service == ServicePhase::Connecting) {
        return kDeviceStateConnecting;
    }
    if (status.conversation == ConversationPhase::Active) {
        return status.turn == TurnPhase::AgentSpeaking ? kDeviceStateSpeaking
                                                       : kDeviceStateListening;
    }
    // Nothing of this projection's own is running. Only a conversation it
    // started is its to end; a lifecycle state belongs to the Application.
    return IsConversationState(current) ? kDeviceStateIdle : current;
}

}  // namespace eidolon
