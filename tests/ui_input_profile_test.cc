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
int main() {
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
