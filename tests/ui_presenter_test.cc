#include <cassert>
#include "application.h"
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
    voice.conversation=ConversationPhase::Closed;presenter.ApplyVoiceStatus(voice);
    assert(presenter.runtime_status().last_transcription.empty());
}
