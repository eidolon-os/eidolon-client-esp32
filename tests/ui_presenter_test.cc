#include <cassert>
#include <cstring>
#include "application.h"
#include "eidolon/eidolon_view.h"
#include "eidolon/eidolon_ui_presenter.h"
#include "eidolon/ui_state_mapper.h"
#include "eidolon/voice_runtime_projector.h"
using namespace eidolon;
int main() {
    Application app;
    EidolonUiPresenter presenter(app);
    presenter.SetRuntimePhase(RuntimePhase::Normal);
    presenter.SetServicePhase(ServicePhase::Registering);
    presenter.SetEnrollmentPhase(EnrollmentPhase::ClaimActive);
    presenter.SetServicePhase(ServicePhase::Connecting);
    assert(!UiStateProjector::AllowsIntent(presenter.runtime_status(),UiIntent::OpenConversation));
    auto voice=VoiceRuntimeProjector::Project({.session=VoiceSessionState::ConfigReady,
        .enrollment=EnrollmentPhase::ClaimActive,.operational_ready=false});
    presenter.ApplyVoiceStatus(voice);
    assert(!UiStateProjector::AllowsIntent(presenter.runtime_status(),UiIntent::OpenConversation));
    voice.service=ServicePhase::Ready;presenter.ApplyVoiceStatus(voice);
    assert(UiStateProjector::AllowsIntent(presenter.runtime_status(),UiIntent::OpenConversation));
    // A policy decision is distinct from waiting for the service to start.
    voice.service=ServicePhase::Preparing;
    voice.service_detail="Ask the Owner to set this device's input and output permissions";
    presenter.ApplyVoiceStatus(voice);
    assert(presenter.runtime_status().service_detail==voice.service_detail);
    voice.service_detail.clear();
    presenter.ApplyVoiceStatus(voice);
    assert(presenter.runtime_status().service_detail.empty());
    voice.service=ServicePhase::Ready;presenter.ApplyVoiceStatus(voice);
    // Late room text must not become the next conversation's first caption.
    presenter.OnTranscription({TranscriptionSource::Agent,"old room",true});
    assert(presenter.runtime_status().last_transcription.empty());
    voice.conversation=ConversationPhase::Active;presenter.ApplyVoiceStatus(voice);
    presenter.OnTranscription({TranscriptionSource::Agent,"Hello",true});
    assert(presenter.runtime_status().last_transcription=="Hello");
    presenter.OnTranscription({TranscriptionSource::Agent,"Hel",false});
    assert(presenter.runtime_status().last_transcription=="Hel");
    presenter.OnTranscription({TranscriptionSource::Agent,"Hello",false});
    assert(presenter.runtime_status().last_transcription=="Hello");
    voice.mic_enabled=false;presenter.ApplyVoiceStatus(voice);
    assert(presenter.tracker().LastTranscription()=="Hello");
    assert(UiStateProjector::Project(presenter.runtime_status()).show_mute_icon);
    presenter.OnAgentPhase(AgentPhase::UserSpeaking);
    assert(presenter.runtime_status().last_transcription.empty());
    presenter.OnTranscription({TranscriptionSource::User,"interim",false});
    assert(presenter.runtime_status().last_transcription.empty());
    presenter.OnTranscription({TranscriptionSource::Agent,"Next response",true});
    // Rejected interruption resumes the existing speech, without a new SDK turn.
    if (!IsPushToTalk(CurrentInteractionMode())) {
        presenter.OnAgentPhase(AgentPhase::AgentSpeaking);
        assert(std::string(UiStateProjector::Project(presenter.runtime_status()).status_text) == "Speaking");
        presenter.OnAgentPhase(AgentPhase::UserSpeaking);
        assert(std::string(UiStateProjector::Project(presenter.runtime_status()).status_text) == "Listening");
        presenter.OnAgentPhase(AgentPhase::AgentSpeaking);
        assert(presenter.tracker().GetPhase() == AgentPhase::AgentSpeaking);
        assert(std::string(UiStateProjector::Project(presenter.runtime_status()).status_text) == "Speaking");
        assert(UiStateProjector::ProjectLegacyDeviceState(
            presenter.runtime_status(), kDeviceStateListening) == kDeviceStateSpeaking);
        // A repeated projected speaking state is harmless (late user-end event).
        presenter.OnAgentPhase(AgentPhase::AgentSpeaking);
        assert(std::string(UiStateProjector::Project(presenter.runtime_status()).status_text) == "Speaking");
        // Accepted cancellation ends playback presentation, then a new reply may start.
        presenter.OnAgentPhase(AgentPhase::Silent);
        assert(UiStateProjector::ProjectLegacyDeviceState(
            presenter.runtime_status(), kDeviceStateSpeaking) == kDeviceStateListening);
        presenter.OnAgentPhase(AgentPhase::AgentSpeaking);
        assert(std::string(UiStateProjector::Project(presenter.runtime_status()).status_text) == "Speaking");
    }
    // A silent semantic decision still terminates the committed PTT round.
    if (IsPushToTalk(CurrentInteractionMode())) {
    presenter.ApplyVoiceStatus(voice);
    presenter.SetPttRecording(false);
    assert(presenter.runtime_status().turn==TurnPhase::Committing);
    presenter.OnAgentPhase(AgentPhase::AwaitingInput);
    assert(presenter.runtime_status().turn==TurnPhase::Idle);
    presenter.OnAgentPhase(AgentPhase::ResponseUnavailable);
    assert(presenter.runtime_status().last_transcription.find("try again")!=std::string::npos);
    presenter.SetPttRecording(true);
    assert(presenter.runtime_status().last_transcription.empty());
    // Late completion cannot clear recording in progress.
    presenter.OnAgentPhase(AgentPhase::ReplyLimitReached);
    assert(presenter.runtime_status().turn==TurnPhase::Recording);
    presenter.SetPttRecording(false);
    presenter.OnAgentPhase(AgentPhase::ReplyLimitReached);
    assert(presenter.runtime_status().turn==TurnPhase::Idle);
    assert(presenter.runtime_status().last_transcription.find("limit")!=std::string::npos);
    }
    voice.conversation=ConversationPhase::Closed;presenter.ApplyVoiceStatus(voice);
    assert(presenter.runtime_status().last_transcription.empty());
    // The key a board registers reaches every profile the presenter projects with.
    assert(!*CurrentUiInputProfile().Key(UiInputSource::SessionButton));
    SetEidolonButtonAvailable(UiInputSource::SessionButton,true,"BOOT");
    assert(!std::strcmp(CurrentUiInputProfile().Key(UiInputSource::SessionButton),"BOOT"));
    assert(!*CurrentUiInputProfile().Key(UiInputSource::TalkButton));
    {
        // Removed while running: the screen names the way back, not a reset.
        EidolonUiPresenter removed(app);
        removed.SetRuntimePhase(RuntimePhase::Normal);
        auto revoked=VoiceRuntimeProjector::Project({.session=VoiceSessionState::Unauthorized,
            .enrollment=EnrollmentPhase::Revoked});
        revoked.enrollment_detail="Removed from this Owner. Hold BOOT to claim it again";
        removed.ApplyVoiceStatus(revoked);
        assert(removed.runtime_status().enrollment==EnrollmentPhase::Revoked);
        assert(removed.runtime_status().enrollment_detail==revoked.enrollment_detail);
        // A later status with nothing to say leaves it in place.
        revoked.enrollment_detail.clear();
        removed.ApplyVoiceStatus(revoked);
        assert(removed.runtime_status().enrollment_detail=="Removed from this Owner. Hold BOOT to claim it again");
    }
}
