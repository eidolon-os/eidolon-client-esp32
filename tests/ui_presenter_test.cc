#include <cassert>
#include <cstring>
#include "application.h"
#include "eidolon/eidolon_view.h"
#include "eidolon/eidolon_ui_presenter.h"
#include "eidolon/ui_state_mapper.h"
#include "eidolon/voice_runtime_projector.h"
#include "esp_timer.h"
using namespace eidolon;

// A person's start or end is drawn at once and kept only while the runtime
// still offers that choice; the runtime's report, or a time limit, takes it down.
static void TestSessionRequests() {
    Application app;
    EidolonUiPresenter p(app);
    SetEidolonTouchAvailable(true);
    p.SetRuntimePhase(RuntimePhase::Normal);
    auto report=[](VoiceSessionState session, EndReason end=EndReason::None) {
        return VoiceRuntimeProjector::Project({.session=session,.enrollment=EnrollmentPhase::ClaimActive,
            .end_reason=end,.operational_ready=true});
    };
    auto shown=[&]() { return UiStateProjector::Project(p.runtime_status(),CurrentUiInputProfile()); };
    auto later=[](int ms) { FakeTimeUs()+=static_cast<int64_t>(ms)*1000; };
    const auto ready=report(VoiceSessionState::ConfigReady);
    p.ApplyVoiceStatus(ready);
    assert(shown().scene==UiScene::Ready && !shown().session_request_pending);

    // T1: a start is drawn as the opening the runtime is about to report.
    DispatchEidolonUiIntent(UiIntent::OpenConversation);
    assert(app.joins==1);
    assert(shown().scene==UiScene::OpeningConversation && shown().session_request_pending);
    assert(shown().end_allowed);   // the opening can be cancelled
    // Only the screen moved: the device's own state is still the runtime's.
    assert(UiStateProjector::ProjectLegacyDeviceState(p.runtime_status(),kDeviceStateIdle)==kDeviceStateIdle);
    // T2: a report that has not answered it yet leaves it drawn; a second start is not offered.
    p.ApplyVoiceStatus(ready);
    assert(shown().scene==UiScene::OpeningConversation);
    DispatchEidolonUiIntent(UiIntent::OpenConversation);
    assert(app.joins==1);
    // T3: the runtime answers, and its report is the screen from then on.
    p.ApplyVoiceStatus(report(VoiceSessionState::Opening));
    assert(p.runtime_status().session_request==SessionRequest::None);
    assert(shown().scene==UiScene::OpeningConversation && !shown().session_request_pending);

    // T4: a start the runtime never answers is taken down after five seconds.
    p.ApplyVoiceStatus(ready);
    later(1000);
    DispatchEidolonUiIntent(UiIntent::OpenConversation);
    assert(app.joins==2 && shown().scene==UiScene::OpeningConversation);
    later(4000);p.Tick();
    assert(shown().scene==UiScene::OpeningConversation);
    later(1100);p.Tick();
    assert(shown().scene==UiScene::Ready && !shown().session_request_pending);

    // T8: an end within half a second of the start is the same finger twice;
    // after that it cancels the opening.
    later(1000);
    DispatchEidolonUiIntent(UiIntent::OpenConversation);
    assert(app.joins==3);
    later(300);
    DispatchEidolonUiIntent(UiIntent::CloseConversation);
    assert(app.leaves==0 && shown().scene==UiScene::OpeningConversation);
    later(300);
    DispatchEidolonUiIntent(UiIntent::CloseConversation);
    assert(app.leaves==1);
    // Nothing was open yet, so the end is already true and nothing is drawn ahead.
    assert(p.runtime_status().session_request==SessionRequest::None && shown().scene==UiScene::Ready);

    // T5: an end is drawn at once: no turn, no caption, and no start until confirmed.
    p.ApplyVoiceStatus(report(VoiceSessionState::InRoom));
    p.OnAgentPhase(AgentPhase::AgentSpeaking);
    p.OnTranscription({TranscriptionSource::Agent,"Hello",true});
    assert(shown().scene==UiScene::Conversation);
    later(1000);
    DispatchEidolonUiIntent(UiIntent::CloseConversation);
    assert(app.leaves==2);
    {
        const auto m=shown();
        assert(m.scene==UiScene::Ended && m.end_reason==EndReason::UserLeft && m.severity==UiSeverity::Normal);
        assert(m.turn==TurnPhase::Idle && !*m.subtitle && m.session_request_pending);
        assert(m.primary_intent==UiIntent::None && !m.primary_enabled && !m.end_allowed && !m.show_end_action);
        assert(m.session_intent==UiIntent::None);
        assert(!UiStateProjector::AllowsIntent(p.runtime_status(),UiIntent::OpenConversation));
    }
    // The device is still in the conversation until the runtime ends it.
    if (!IsPushToTalk(CurrentInteractionMode()))
        assert(UiStateProjector::ProjectLegacyDeviceState(p.runtime_status(),kDeviceStateSpeaking)==kDeviceStateSpeaking);
    // T6: the conversation's own news while the end is pending does not bring it back.
    p.OnAgentPhase(AgentPhase::AgentThinking);
    p.OnTranscription({TranscriptionSource::Agent,"More",true});
    assert(shown().scene==UiScene::Ended && !*shown().subtitle);
    DispatchEidolonUiIntent(UiIntent::OpenConversation);
    assert(app.joins==3);
    // T7: the runtime confirms; a start in the same half second is the same finger.
    later(100);
    p.ApplyVoiceStatus(report(VoiceSessionState::ConfigReady,EndReason::UserLeft));
    assert(p.runtime_status().session_request==SessionRequest::None);
    assert(shown().scene==UiScene::Ended && shown().primary_intent==UiIntent::OpenConversation);
    DispatchEidolonUiIntent(UiIntent::OpenConversation);
    assert(app.joins==3);
    later(500);
    DispatchEidolonUiIntent(UiIntent::OpenConversation);
    assert(app.joins==4 && shown().scene==UiScene::OpeningConversation);

    // T9: an end the runtime never answers is taken down after ten seconds.
    p.ApplyVoiceStatus(report(VoiceSessionState::InRoom));
    assert(p.runtime_status().session_request==SessionRequest::None);
    later(1000);
    DispatchEidolonUiIntent(UiIntent::CloseConversation);
    assert(app.leaves==3 && shown().scene==UiScene::Ended);
    later(9000);p.Tick();
    assert(shown().scene==UiScene::Ended);
    later(1100);p.Tick();
    assert(shown().scene==UiScene::Conversation && !shown().session_request_pending);

    // T10: setup taking over is the runtime moving on; the start is not drawn back after it.
    p.ApplyVoiceStatus(ready);
    later(1000);
    DispatchEidolonUiIntent(UiIntent::OpenConversation);
    assert(app.joins==5 && shown().scene==UiScene::OpeningConversation);
    p.SetRuntimePhase(RuntimePhase::Commissioning,"Ready for secure device setup");
    assert(shown().scene==UiScene::Commissioning && p.runtime_status().session_request==SessionRequest::None);
    p.SetRuntimePhase(RuntimePhase::Normal);
    assert(shown().scene==UiScene::Ready);

    // T11: the physical session button is drawn ahead the same way.
    later(1000);
    DispatchEidolonUiInput(UiInputSource::SessionButton,UiInputGesture::Click);
    assert(app.joins==6 && shown().scene==UiScene::OpeningConversation && shown().session_request_pending);
}

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
    TestSessionRequests();
}
