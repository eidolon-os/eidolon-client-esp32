#include "eidolon/output_policy.h"
#include <cassert>
#include <cJSON.h>
#include <fstream>
#include <sstream>
using namespace eidolon;
using presentation::Output;
int main() {
#if CONFIG_EIDOLON_OUTPUT_POLICY_V1
    static_assert(kOutputPolicyRequired);
    assert(!CurrentOutputGate().Allows(Output::Speech));
#endif
    DeviceOutputGate gate(false);
    assert(!gate.Allows(Output::Speech));
    const auto expression=OutputBit(Output::Expression);
    const auto speech=OutputBit(Output::Speech);
    assert(gate.Bind({true,1,expression|speech}));
    SessionOutputPlan plan{"session-1",1,expression,true};
    assert(!gate.Start(plan,"other"));
    assert(gate.Start(plan,"session-1"));
    assert(gate.Allows(Output::Expression));
    assert(!gate.Allows(Output::Speech));
    assert(gate.Bind({true,1,expression|speech}));
    assert(gate.Allows(Output::Expression));
    assert(!gate.Bind({true,1,speech}));
    assert(!gate.Allows(Output::Expression));
    assert(gate.Bind({true,2,speech}));
    assert(!gate.Start(plan,"session-1"));
    assert(!gate.Bind({}));
    plan={"session-2",2,speech,false};
    assert(gate.Start(plan,"session-2"));
    assert(gate.Allows(Output::Speech));
    gate.Close();
    assert(!gate.Allows(Output::Speech));
    const auto cue=OutputBit(Output::AudioCue);
    DeviceOutputGate cue_only(false,expression|speech|cue);
    assert(cue_only.Bind({true,1,expression|cue}));
    assert(cue_only.Start({"cue",1,expression|cue,true},"cue"));
    assert(cue_only.Allows(Output::AudioCue));
    assert(!cue_only.Allows(Output::Speech));
    assert(cue_only.AllowsAny(kAudioOutputs));
    // Revoking the cue also closes playback, without changing expression.
    assert(cue_only.Bind({true,2,expression}));
    assert(!cue_only.AllowsAny(kAudioOutputs));
    assert(!cue_only.Start({"cue",2,expression|cue,true},"cue"));
    assert(cue_only.Start({"silent",2,expression,true},"silent"));
    assert(cue_only.Allows(Output::Expression));
    assert(!cue_only.AllowsAny(kAudioOutputs));
    const auto dialogue=OutputBit(Output::DialogueText);
    DeviceOutputGate text_only(false,dialogue);
    assert(text_only.Bind({true,1,dialogue|speech|expression}));
    assert(text_only.Start({"text",1,dialogue,false},"text"));
    assert(text_only.Allows(Output::DialogueText));
    // A valid server grant cannot manufacture a missing local executor.
    assert(!text_only.Start({"text",1,speech,false},"text"));
    assert(!text_only.Allows(Output::DialogueText));
    assert(!text_only.Start({"text",1,expression,true},"text"));
    DeviceOutputGate legacy_text(true,dialogue);
    assert(legacy_text.Allows(Output::DialogueText));
    assert(!legacy_text.Allows(Output::Speech));
    assert(!legacy_text.Allows(Output::AudioCue));
    DeviceOutputGate legacy(true);
    assert(legacy.Allows(Output::Speech));
    assert(!legacy.Allows(Output::Expression));
    assert(legacy.Bind({true,1,0}));
    assert(!legacy.Allows(Output::Speech));
    assert(!legacy.Start({"s",1,0,false},"s"));
    for (const char* raw : {R"({"revision":true,"allowed":{}})",
            R"({"revision":1,"allowed":{"speech":1}})",
            R"({"revision":1,"allowed":{"speech":true,"speech":false}})",
            R"({"revision":1,"revision":2,"allowed":{}})",
            R"({"revision":1,"allowed":{},"unknown":false})",
            R"({"schema_version":1.1,"revision":1,"allowed":{}})"}) {
        auto* json=cJSON_Parse(raw); DeviceOutputPolicy policy;
        assert(!ParseOutputPolicy(json,policy)); cJSON_Delete(json);
    }
    std::ifstream file("tests/fixtures/presentation/silent-session.json");
    std::ostringstream buffer; buffer<<file.rdbuf();
    auto* json=cJSON_Parse(buffer.str().c_str());
    assert(ParseSessionOutputPlan(json,plan)); cJSON_Delete(json);
    assert(plan.face_profile && plan.selected==expression && plan.session_id=="session-1");
    auto* server_plan=cJSON_Parse(
        R"({"schema_version":1,"session_id":"session-1","policy_revision":1,"outputs":{"speech":false,"dialogue_text":false,"expression":true,"audio_cue":false,"motion":false},"inputs":{"microphone":true},"expression_profile":"eidolon.face.v1"})");
    assert(ParseSessionOutputPlan(server_plan, plan));
    cJSON_Delete(server_plan);
}
