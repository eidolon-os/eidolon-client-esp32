#include <cassert>
#include <cstring>
#include "eidolon/ui_state_mapper.h"
using namespace eidolon;
static EidolonRuntimeStatus Ready(InteractionMode mode) {
    EidolonRuntimeStatus s;
    s.runtime=RuntimePhase::Normal;s.enrollment=EnrollmentPhase::ClaimActive;
    s.service=ServicePhase::Ready;s.interaction_mode=mode;
    return s;
}
// A session control means the session button's click, in every state: the
// on-screen one a panel draws resolves through the same bindings.
static void SessionControlIsTheSessionButton() {
    UiInputProfile input;
    input.enabled_inputs=input.available_inputs=InputBit(UiInputSource::Touch)|InputBit(UiInputSource::SessionButton);
    for (auto mode:{InteractionMode::HalfDuplex,InteractionMode::FullDuplex,InteractionMode::PushToTalk})
    for (int r=0;r<=static_cast<int>(RuntimePhase::Fault);++r)
    for (int e=0;e<=static_cast<int>(EnrollmentPhase::Revoked);++e)
    for (int v=0;v<=static_cast<int>(ServicePhase::Fault);++v)
    for (int c=0;c<=static_cast<int>(ConversationPhase::Failed);++c)
    for (auto turn:{TurnPhase::Idle,TurnPhase::UserSpeaking,TurnPhase::AgentThinking,TurnPhase::Recording}) {
        EidolonRuntimeStatus s;
        s.runtime=static_cast<RuntimePhase>(r);s.enrollment=static_cast<EnrollmentPhase>(e);
        s.service=static_cast<ServicePhase>(v);s.conversation=static_cast<ConversationPhase>(c);
        s.turn=turn;s.interaction_mode=mode;
        const UiIntent button=UiStateProjector::ResolveInput(s,input,UiInputSource::SessionButton,UiInputGesture::Click);
        assert(UiStateProjector::SessionControlIntent(s,input)==button);
        assert(UiStateProjector::Project(s,input).session_intent==button);
    }
    auto s=Ready(InteractionMode::HalfDuplex);
    assert(UiStateProjector::SessionControlIntent(s,input)==UiIntent::OpenConversation);
    s.conversation=ConversationPhase::Opening;
    assert(UiStateProjector::SessionControlIntent(s,input)==UiIntent::CloseConversation);
    s.conversation=ConversationPhase::Active;
    // Ends the conversation, where the primary touch action is the mute toggle.
    assert(UiStateProjector::SessionControlIntent(s,input)==UiIntent::CloseConversation);
    assert(UiStateProjector::Project(s,input).primary_intent==UiIntent::ToggleMicrophone);
    s.conversation=ConversationPhase::Closed;s.service=ServicePhase::Reconnecting;
    assert(UiStateProjector::SessionControlIntent(s,input)==UiIntent::None);
    // An on-screen control does not depend on the physical button being there.
    input.available_inputs=InputBit(UiInputSource::Touch);
    s=Ready(InteractionMode::HalfDuplex);
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::SessionButton,UiInputGesture::Click)==UiIntent::None);
    assert(UiStateProjector::SessionControlIntent(s,input)==UiIntent::OpenConversation);
}

// A product that offers no mute: nothing may ask for one, however it is
// touched or pressed, and the session control is unaffected.
static void ProductWithoutMute() {
    UiInputProfile input;
    input.enabled_inputs=input.available_inputs=InputBit(UiInputSource::Touch)|
        InputBit(UiInputSource::SessionButton)|InputBit(UiInputSource::AuxiliaryButton);
    input.microphone_mute=false;
    auto s=Ready(InteractionMode::HalfDuplex);
    s.conversation=ConversationPhase::Active;
    for (auto source:{UiInputSource::Touch,UiInputSource::AuxiliaryButton})
        assert(!input.Binding(UiIntent::ToggleMicrophone,source));
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::AuxiliaryButton,UiInputGesture::DoubleClick)==UiIntent::None);
    assert(UiStateProjector::SessionControlIntent(s,input)==UiIntent::CloseConversation);
    assert(UiStateProjector::Project(s,input).primary_presentation!=UiActionPresentation::TouchControl);
    // The same profile with a mute keeps offering it.
    input.microphone_mute=true;
    assert(input.Binding(UiIntent::ToggleMicrophone,UiInputSource::Touch));
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::AuxiliaryButton,UiInputGesture::DoubleClick)==UiIntent::ToggleMicrophone);
}

int main() {
    SessionControlIsTheSessionButton();
    ProductWithoutMute();
    UiInputProfile input;
    auto initial=Ready(InteractionMode::HalfDuplex);
    assert(!std::strcmp(UiStateProjector::Project(initial,input).detail_text,"No start input available"));
    input.automatic_start=true;
    assert(!std::strcmp(UiStateProjector::Project(initial,input).detail_text,"Waiting for automatic connection"));
    input.automatic_start=false;
    input.enabled_inputs=InputBit(UiInputSource::Touch)|InputBit(UiInputSource::SessionButton);
    input.available_inputs=InputBit(UiInputSource::SessionButton);
    auto s=Ready(InteractionMode::FullDuplex);
    auto m=UiStateProjector::Project(s,input);
    assert(m.primary_presentation==UiActionPresentation::InputHint);
    assert(!std::strcmp(m.input_hint,"Press button to start"));
    assert(!m.show_end_action);
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::SessionButton,UiInputGesture::Click)==UiIntent::OpenConversation);
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::Touch,UiInputGesture::Click)==UiIntent::None);
    s.conversation=ConversationPhase::Active;
    m=UiStateProjector::Project(s,input);
    assert(!m.show_end_action && m.end_allowed);
    assert(!std::strcmp(m.input_hint,"Press button to end"));
    assert(UiStateProjector::AllowsIntent(s,UiIntent::CloseConversation));
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::SessionButton,UiInputGesture::Click)==UiIntent::CloseConversation);
    assert(!UiStateProjector::AllowsIntent(s,UiIntent::BeginTalk));
    input.available_inputs |= InputBit(UiInputSource::Touch);
    m=UiStateProjector::Project(s,input);
    assert(m.primary_presentation==UiActionPresentation::TouchControl && m.show_end_action);
    assert(m.primary_intent==UiIntent::ToggleMicrophone);
    // Detection alone cannot enable touch the product did not declare.
    input.enabled_inputs=InputBit(UiInputSource::SessionButton);
    assert(UiStateProjector::Project(s,input).primary_presentation==UiActionPresentation::InputHint);
    // A fourth device: physical PTT and session buttons, no touch, no board-name branch.
    input.enabled_inputs=input.available_inputs=InputBit(UiInputSource::SessionButton)|InputBit(UiInputSource::TalkButton);
    s.interaction_mode=InteractionMode::PushToTalk;
    m=UiStateProjector::Project(s,input);
    assert(!std::strcmp(m.input_hint,"Hold button to talk"));
    s.conversation=ConversationPhase::Closed;
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::TalkButton,UiInputGesture::Press)==UiIntent::OpenConversation);
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::TalkButton,UiInputGesture::Release)==UiIntent::None);
    s.conversation=ConversationPhase::Active;
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::TalkButton,UiInputGesture::Press)==UiIntent::BeginTalk);
    s.turn=TurnPhase::Recording;
    assert(!std::strcmp(UiStateProjector::Project(s,input).input_hint,"Release button to send"));
    for (auto event:{UiInputGesture::Release,UiInputGesture::Cancel})
        assert(UiStateProjector::ResolveInput(s,input,UiInputSource::TalkButton,event)==UiIntent::CommitTalk);
    input.available_inputs=0;
    m=UiStateProjector::Project(s,input);
    assert(m.primary_presentation==UiActionPresentation::Hidden && !m.show_end_action);
    // Losing input does not make the semantic termination operation illegal.
    assert(UiStateProjector::AllowsIntent(s,UiIntent::CommitTalk));
    s.runtime=RuntimePhase::Commissioning;
    input.available_inputs=input.enabled_inputs;
    assert(UiStateProjector::ResolveInput(s,input,UiInputSource::SessionButton,UiInputGesture::Click)==UiIntent::None);
}
