#include "eidolon_ui_presenter.h"

#include <esp_log.h>
#include <cstring>

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "eidolon_ui_labels.h"
#include "eidolon_view.h"
#include "ui_state_mapper.h"

#define TAG "EidolonUiPresenter"

namespace eidolon {

EidolonUiPresenter::EidolonUiPresenter(Application& app) : app_(app)
{
    SetEidolonUiIntentHandler([this](UiIntent intent) {
        app_.Schedule([this, intent]() {
            // View events are pointer operations, never a substitute for a
            // physical binding or a successful input-driver registration.
            if (CurrentUiInputProfile().Binding(intent,UiInputSource::Touch)) HandleIntent(intent);
        });
    });
    SetEidolonUiInputHandler([this](UiInputSource source, UiInputGesture gesture) {
        app_.Schedule([this,source,gesture]() {
            HandleIntent(UiStateProjector::ResolveInput(status_,CurrentUiInputProfile(),source,gesture));
        });
    });
    SetEidolonInputAvailabilityHandler([this](UiInputSource source, bool available) {
        app_.Schedule([this,source,available]() {
            // Input loss must not leave a held PTT recording open, even when
            // another input remains available but never owned this gesture.
            if (!available && status_.turn==TurnPhase::Recording &&
                (source==UiInputSource::Touch || source==UiInputSource::TalkButton))
                HandleIntent(UiIntent::CommitTalk);
            Reapply();
        });
    });
}

EidolonUiPresenter::~EidolonUiPresenter()
{
    SetEidolonUiIntentHandler({});
    SetEidolonUiInputHandler({});
    SetEidolonInputAvailabilityHandler({});
}

void EidolonUiPresenter::ApplyVoiceStatus(const VoiceRuntimeStatus& voice)
{
    const bool entered_conversation = status_.conversation != ConversationPhase::Active &&
                                      voice.conversation == ConversationPhase::Active;
    if (status_.enrollment != voice.enrollment) {
        status_.enrollment_detail.clear();
    }
    if (status_.service != voice.service) {
        status_.service_detail.clear();
    }
    status_.enrollment = voice.enrollment;
    status_.service = voice.service;
    status_.service_detail = voice.service_detail;
    status_.conversation = voice.conversation;
    status_.end_reason = voice.end_reason;
    status_.mic_enabled = voice.mic_enabled;
    if (voice.conversation != ConversationPhase::Active) {
        status_.turn = TurnPhase::Idle;
        status_.last_transcription.clear();
        status_.last_transcription_role = "system";
        tracker_.OnRoomDisconnected();
    } else if (entered_conversation) {
        tracker_.OnRoomConnected();
    }
    Reapply();
}

void EidolonUiPresenter::SetRuntimePhase(RuntimePhase phase, const std::string& detail)
{
    if (status_.runtime == phase && status_.runtime_detail == detail) {
        return;
    }
    status_.runtime = phase;
    status_.runtime_detail = detail;
    Reapply();
}

void EidolonUiPresenter::SetEnrollmentPhase(EnrollmentPhase phase,
                                             const std::string& detail)
{
    if (status_.enrollment == phase && status_.enrollment_detail == detail) {
        return;
    }
    status_.enrollment = phase;
    status_.enrollment_detail = detail;
    Reapply();
}

void EidolonUiPresenter::SetServicePhase(ServicePhase phase, const std::string& detail)
{
    if (status_.service == phase && status_.service_detail == detail) {
        return;
    }
    status_.service = phase;
    status_.service_detail = detail;
    Reapply();
}

void EidolonUiPresenter::OnTranscription(const TranscriptionEvent& event)
{
    if (status_.conversation != ConversationPhase::Active || event.text.empty() ||
        (event.source == TranscriptionSource::User && !event.is_final)) {
        return;
    }
    tracker_.OnTranscription(event);
    status_.last_transcription = event.text;
    switch (event.source) {
    case TranscriptionSource::User:
        status_.last_transcription_role = "user";
        break;
    case TranscriptionSource::Agent:
        status_.last_transcription_role = "assistant";
        break;
    case TranscriptionSource::System:
    case TranscriptionSource::Unknown:
    default:
        status_.last_transcription_role = "system";
        break;
    }
    Reapply();
}

void EidolonUiPresenter::OnAgentPhase(AgentPhase phase)
{
    tracker_.OnAgentPhase(phase);
    if (status_.conversation != ConversationPhase::Active ||
        status_.turn == TurnPhase::Recording) {
        return;
    }
    if ((phase == AgentPhase::UserSpeaking || phase == AgentPhase::AgentThinking) &&
        std::strcmp(status_.last_transcription_role, "assistant") == 0) {
        status_.last_transcription.clear();
    }
    switch (phase) {
    case AgentPhase::AwaitingInput:
        status_.turn = TurnPhase::Idle;
        break;
    case AgentPhase::ResponseUnavailable:
    case AgentPhase::ReplyLimitReached:
        status_.turn = TurnPhase::Idle;
        status_.last_transcription_role = "system";
        status_.last_transcription = phase == AgentPhase::ResponseUnavailable
            ? "No response available. Hold to try again."
            : "Reply limit reached. Hold to continue.";
        break;
    case AgentPhase::UserSpeaking:
        status_.turn = TurnPhase::UserSpeaking;
        break;
    case AgentPhase::AgentThinking:
        status_.turn = TurnPhase::AgentThinking;
        break;
    case AgentPhase::AgentSpeaking:
        status_.turn = TurnPhase::AgentSpeaking;
        break;
    case AgentPhase::Silent:
        if (status_.turn != TurnPhase::Committing) {
            status_.turn = TurnPhase::Idle;
        }
        break;
    }
    Reapply();
}

void EidolonUiPresenter::OnPttTurnStatus(const std::string& outcome)
{
    if (!IsPushToTalk(status_.interaction_mode)) {
        ESP_LOGW(TAG, "ignored PTT status in automatic mode outcome=%s",
                 outcome.c_str());
        return;
    }
    if (outcome == "recording") {
        status_.turn = TurnPhase::Recording;
    } else if (outcome == "finalizing" || outcome == "committed") {
        status_.turn = TurnPhase::Committing;
    } else if (outcome.rfind("rejected:", 0) == 0 ||
               outcome.rfind("cancelled:", 0) == 0) {
        status_.turn = TurnPhase::Idle;
        tracker_.OnAgentPhase(AgentPhase::Silent);
    }
    Reapply();
}

void EidolonUiPresenter::OnPresenceWakePhase(PresenceWakePhase phase)
{
    status_.presence_wake = phase;
    Reapply();
}

void EidolonUiPresenter::SetPttRecording(bool recording)
{
    if (!IsPushToTalk(status_.interaction_mode)) {
        ESP_LOGW(TAG, "ignored local PTT state in automatic mode recording=%d",
                 recording ? 1 : 0);
        return;
    }
    status_.turn = recording ? TurnPhase::Recording : TurnPhase::Committing;
    if (recording && std::strcmp(status_.last_transcription_role, "system") == 0) {
        status_.last_transcription.clear();
    }
    Reapply();
}

void EidolonUiPresenter::HandleIntent(UiIntent intent)
{
    if (!UiStateProjector::AllowsIntent(status_, intent)) {
        ESP_LOGI(TAG, "ignored UI intent=%d scene=%d", static_cast<int>(intent),
                 static_cast<int>(UiStateProjector::Project(status_).scene));
        return;
    }
    switch (intent) {
    case UiIntent::OpenConversation:
        app_.RequestVoiceJoin();
        break;
    case UiIntent::CloseConversation:
        app_.RequestVoiceLeave();
        break;
    case UiIntent::BeginTalk:
        app_.PttPress();
        break;
    case UiIntent::CommitTalk:
        app_.PttRelease();
        break;
    case UiIntent::ToggleMicrophone:
        app_.ToggleMicrophone();
        break;
    case UiIntent::OpenSetup:
        OpenEidolonSetup();
        break;
    case UiIntent::None:
    default:
        break;
    }
}

void EidolonUiPresenter::Reapply()
{
    SyncLegacyDeviceState();
    ApplyModel(UiStateProjector::Project(status_,CurrentUiInputProfile()));
}

void EidolonUiPresenter::SyncLegacyDeviceState()
{
    const DeviceState current = app_.GetDeviceState();
    const DeviceState target =
        UiStateProjector::ProjectLegacyDeviceState(status_, current);
    if (current != target) {
        app_.SetDeviceState(target);
    }
}

void EidolonUiPresenter::ApplyModel(const EidolonUiModel& model)
{
    if (last_visible_state_ != model.state_label) {
        ESP_LOGI(TAG, "visible state=%s scene=%d", model.state_label,
                 static_cast<int>(model.scene));
        last_visible_state_ = model.state_label;
    }
    if (auto* view = GetEidolonView()) {
        view->Render(model);
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (!display) {
        return;
    }
    display->SetVoiceChrome(model.mode_label, model.state_label, "EXIT",
                            model.show_end_action);
    display->SetEmotion(model.emotion);
    display->SetStatus(model.status_text);
    display->SetChatMessage(model.subtitle[0] == '\0' ? "system" : model.subtitle_role,
                            model.subtitle[0] == '\0' ? model.detail_text : model.subtitle);
}

}  // namespace eidolon
