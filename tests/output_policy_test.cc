#include "eidolon/output_policy.h"
#include <cassert>
#include <cJSON.h>
#include <fstream>
#include <sstream>
using namespace eidolon;
using presentation::Output;
int main() {
    // SDK serializes an absent input override as null on existing policies.
    for (const char* raw : {
        R"({"schema_version":1,"revision":1,"allowed":{"speech":true},"inputs":null})",
        R"({"schema_version":1,"revision":1,"allowed":{"speech":true},"inputs":{"microphone":false}})"
    }) {
        auto* json=cJSON_Parse(raw); DeviceOutputPolicy policy;
        assert(ParseOutputPolicy(json,policy)); cJSON_Delete(json);
    }
#if CONFIG_EIDOLON_OUTPUT_POLICY_V1
    static_assert(kOutputPolicyRequired);
    assert(!CurrentOutputGate().Allows(Output::Speech));
#endif
    DeviceOutputGate gate(false);
    assert(!gate.Allows(Output::Speech));
    const auto expression=OutputBit(Output::Expression);
    const auto speech=OutputBit(Output::Speech);
    DeviceOutputGate input_gate(false);
    DeviceOutputPolicy receive_only{true,1,speech,true,false};
    assert(input_gate.Bind(receive_only));
    assert(!input_gate.AllowsMicrophone());
    SessionOutputPlan listening{"listen",1,speech,false,true};
    assert(!input_gate.Start(listening,"listen")); // no input escalation
    listening.microphone=false;
    assert(input_gate.Start(listening,"listen"));
    assert(input_gate.Allows(Output::Speech)); // input and output are independent
    assert(!input_gate.AllowsMicrophone());
    auto enabled=receive_only;enabled.microphone=true;
    assert(!input_gate.Bind(enabled)); // cannot reuse revision with new inputs
    enabled.revision=2;
    assert(input_gate.Bind(enabled));
    listening.policy_revision=2;
    listening.microphone=true;
    assert(input_gate.Start(listening,"listen"));
    assert(input_gate.AllowsMicrophone());
    input_gate.Close();
    assert(!input_gate.AllowsMicrophone());
    for (const char* raw : {
        R"({"revision":1,"allowed":{},"inputs":{"microphone":1}})",
        R"({"revision":1,"allowed":{},"inputs":{"microphone":true,"microphone":false}})",
        R"({"revision":1,"allowed":{},"inputs":{"camera":true}})"
    }) {
        auto* json=cJSON_Parse(raw);DeviceOutputPolicy policy;
        assert(!ParseOutputPolicy(json,policy));cJSON_Delete(json);
    }
    for (const char* raw : {
        R"({"schema_version":1,"session_id":"listen","policy_revision":2,"outputs":{"speech":true},"inputs":{"microphone":false}})",
        R"({"schema_version":1,"session_id":"listen","policy_revision":2,"outputs":{"speech":true},"inputs":{}})"
    }) {
        auto* json=cJSON_Parse(raw);SessionOutputPlan parsed;
        assert(ParseSessionOutputPlan(json,parsed));cJSON_Delete(json);
        assert(!parsed.microphone);
    }
    auto* invalid_inputs=cJSON_Parse(R"({"session_id":"listen","policy_revision":2,"outputs":{"speech":true},"inputs":null})");
    assert(!ParseSessionOutputPlan(invalid_inputs,listening));cJSON_Delete(invalid_inputs);
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
    assert(!legacy.Start({"s",1,0,false,false},"s"));
    // A prepared input endpoint captures speech without enabling any output.
    std::ifstream input_file("tests/fixtures/presentation/input-only-session.json");
    std::ostringstream input_buffer; input_buffer<<input_file.rdbuf();
    auto* input_json=cJSON_Parse(input_buffer.str().c_str());
    SessionOutputPlan input_plan;
    assert(ParseSessionOutputPlan(input_json,input_plan));cJSON_Delete(input_json);
    DeviceOutputGate capture_only(false);
    assert(capture_only.Bind({true,2,speech,true,true}));
    assert(capture_only.Start(input_plan,"input-session"));
    assert(capture_only.AllowsMicrophone());
    assert(!capture_only.AllowsAny(UINT32_MAX));
    capture_only.Close();
    assert(!capture_only.AllowsMicrophone());
    assert(capture_only.Bind({true,3,speech,true,false}));
    input_plan.policy_revision=3;
    assert(!capture_only.Start(input_plan,"input-session"));
    auto* empty_plan=cJSON_Parse(R"({"session_id":"empty","policy_revision":1,"outputs":{},"inputs":{"microphone":false}})");
    assert(!ParseSessionOutputPlan(empty_plan,input_plan));cJSON_Delete(empty_plan);
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
    assert(ParseSessionOutputPlan(json,plan));
    cJSON_AddStringToObject(json,"motion_profile","stackchan.head.v1");
    assert(ParseSessionOutputPlan(json,plan));
    cJSON_ReplaceItemInObjectCaseSensitive(json,"motion_profile",cJSON_CreateString("unsupported"));
    assert(!ParseSessionOutputPlan(json,plan));
    cJSON_Delete(json);
    assert(plan.face_profile && plan.selected==expression && plan.session_id=="session-1");
}
