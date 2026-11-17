#include "eidolon/output_policy.h"
#include <cassert>
#include <cJSON.h>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
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
    {
        // Ambient local life: Owner policy outside a session, the plan inside one.
        const auto motion=OutputBit(Output::Motion);
        DeviceOutputGate ambient(false);
        assert(!ambient.AllowsAmbient(Output::Motion));            // unknown policy, strict profile
        DeviceOutputPolicy owner{true,1,motion|OutputBit(Output::Speech),true,true};
        assert(ambient.Bind(owner));
        assert(ambient.AllowsAmbient(Output::Motion));             // no session: standing policy
        assert(!ambient.Allows(Output::Motion));                   // session-scoped path unchanged
        SessionOutputPlan quiet{"q",1,OutputBit(Output::Speech),false,true};
        assert(ambient.Start(quiet,"q"));
        assert(!ambient.AllowsAmbient(Output::Motion));            // the plan narrows it
        ambient.Suspend();
        assert(!ambient.AllowsAmbient(Output::Motion));
        ambient.Close();
        assert(ambient.AllowsAmbient(Output::Motion));
        DeviceOutputPolicy no_motion{true,2,OutputBit(Output::Speech),true,true};
        assert(ambient.Bind(no_motion));
        assert(!ambient.AllowsAmbient(Output::Motion));            // Owner revoked it
        DeviceOutputGate unsupported(false, OutputBit(Output::Speech));
        assert(unsupported.Bind(owner));
        assert(!unsupported.AllowsAmbient(Output::Motion));        // not installed
        DeviceOutputGate legacy(true);
        assert(legacy.AllowsAmbient(Output::Speech)==legacy.Allows(Output::Speech));
    }
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
    for (int i=0;i<100;++i) {
        input_gate.Suspend();
        assert(!input_gate.AllowsMicrophone() && !input_gate.Allows(Output::Speech));
        assert(input_gate.CanResume("listen"));
        assert(!input_gate.Resume("other"));
        assert(input_gate.Bind(enabled)); // equal snapshots retain suspended grant
        assert(!input_gate.AllowsMicrophone());
        assert(input_gate.Resume("listen"));
        assert(input_gate.AllowsMicrophone() && input_gate.Allows(Output::Speech));
    }
    input_gate.Close();
    assert(!input_gate.AllowsMicrophone());
    assert(!input_gate.Resume("listen")); // terminal close cannot be resurrected
    assert(input_gate.Start(listening,"listen"));
    input_gate.Suspend();
    enabled.revision=3;
    assert(input_gate.Bind(enabled));
    assert(!input_gate.CanResume("listen") && !input_gate.Resume("listen"));
    listening.policy_revision=3;
    assert(input_gate.Start(listening,"listen"));
    input_gate.Suspend();
    auto stale=enabled;stale.revision=2;
    assert(!input_gate.Bind(stale));
    assert(!input_gate.Resume("listen")); // rejected policy also invalidates grant
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
    {
        // The Owner's camera use (SDK CameraUse) rides in the same inputs. Before
        // the firmware read it, a policy carrying it was refused whole — the
        // microphone and every output with it.
        const std::pair<const char*,CameraUse> uses[]={
            {R"({"revision":4,"allowed":{"speech":true},"inputs":{"microphone":true,"camera":"capture"}})",CameraUse::Capture},
            {R"({"revision":4,"allowed":{"speech":true},"inputs":{"microphone":true,"camera":"local"}})",CameraUse::Local},
            {R"({"revision":4,"allowed":{"speech":true},"inputs":{"camera":"off","microphone":false}})",CameraUse::Off},
            {R"({"revision":4,"allowed":{"speech":true},"inputs":{"microphone":true,"camera":null}})",CameraUse::Unset},
            {R"({"revision":4,"allowed":{"speech":true},"inputs":{"microphone":true}})",CameraUse::Unset},
        };
        for (const auto& [raw,use] : uses) {
            auto* json=cJSON_Parse(raw);DeviceOutputPolicy policy;
            assert(ParseOutputPolicy(json,policy));cJSON_Delete(json);
            assert(policy.known && policy.inputs_known && policy.camera==use);
            DeviceOutputGate camera_gate(false);
            assert(!camera_gate.AllowsCameraCapture());   // no policy bound yet
            assert(camera_gate.Bind(policy));
            assert(camera_gate.AllowsCameraCapture()==(use==CameraUse::Capture));
        }
        for (const char* raw : {
            R"({"revision":1,"allowed":{},"inputs":{"camera":"video"}})",
            R"({"revision":1,"allowed":{},"inputs":{"camera":"Capture"}})",
            R"({"revision":1,"allowed":{},"inputs":{"camera":"capture","camera":"off"}})",
            R"({"revision":1,"allowed":{},"inputs":{"camera":1}})"
        }) {
            auto* json=cJSON_Parse(raw);DeviceOutputPolicy policy;
            assert(!ParseOutputPolicy(json,policy));cJSON_Delete(json);
        }
        // What the device keeps across a reboot reads back as the same policy,
        // camera use included: a restored policy that lost it would refuse
        // every still until the next configuration refresh.
        for (CameraUse use : {CameraUse::Unset,CameraUse::Off,CameraUse::Local,CameraUse::Capture}) {
            DeviceOutputPolicy kept{true,9,speech|OutputBit(Output::Expression),true,true,use};
            auto* root=cJSON_CreateObject();
            assert(AddOutputPolicyJson(root,"output_policy",kept));
            DeviceOutputPolicy restored;
            assert(ParseOutputPolicy(cJSON_GetObjectItemCaseSensitive(root,"output_policy"),restored));
            assert(restored==kept);
            cJSON_Delete(root);
        }
        {
            auto* root=cJSON_CreateObject();
            assert(AddOutputPolicyJson(root,"output_policy",DeviceOutputPolicy{}));
            assert(cJSON_GetObjectItemCaseSensitive(root,"output_policy")==nullptr);
            cJSON_Delete(root);
        }
        // Narrowing the camera is a new revision, and the next still is refused.
        DeviceOutputGate camera_gate(false);
        DeviceOutputPolicy capture{true,5,speech,true,true,CameraUse::Capture};
        assert(camera_gate.Bind(capture) && camera_gate.AllowsCameraCapture());
        auto narrowed=capture;narrowed.camera=CameraUse::Local;
        assert(!camera_gate.Bind(narrowed));               // same revision, different inputs
        narrowed.revision=6;
        assert(camera_gate.Bind(narrowed) && !camera_gate.AllowsCameraCapture());
        auto* plan_json=cJSON_Parse(R"({"schema_version":1,"session_id":"look","policy_revision":6,"outputs":{"speech":true},"inputs":{"microphone":true,"camera":"capture"},"device_capabilities":["camera.capture"]})");
        SessionOutputPlan look;
        assert(ParseSessionOutputPlan(plan_json,look));cJSON_Delete(plan_json);
        assert(look.camera==CameraUse::Capture && look.device_capabilities.size()==1);
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
    legacy.Suspend();
    assert(!legacy.AllowsMicrophone() && !legacy.Allows(Output::Speech));
    assert(!legacy.Resume(""));
    assert(legacy.Resume("legacy-session"));
    assert(legacy.AllowsMicrophone() && legacy.Allows(Output::Speech));
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

    // A plan lists the installed device capabilities this session may invoke;
    // the gate keeps them for exactly as long as the session is active.
    SessionOutputPlan with_settings;
    auto* settings_json=cJSON_Parse(R"({"schema_version":1,"session_id":"talk","policy_revision":1,)"
        R"("outputs":{"speech":true},"expression_profile":null,"motion_profile":null,)"
        R"("device_capabilities":["audio.volume.set","audio.volume.adjust"]})");
    assert(ParseSessionOutputPlan(settings_json,with_settings));
    cJSON_Delete(settings_json);
    assert((with_settings.device_capabilities==std::vector<std::string>{"audio.volume.set","audio.volume.adjust"}));
    for (const char* raw : {
            R"({"schema_version":1,"session_id":"talk","policy_revision":1,"outputs":{"speech":true},"device_capabilities":"audio.volume.set"})",
            R"({"schema_version":1,"session_id":"talk","policy_revision":1,"outputs":{"speech":true},"device_capabilities":[1]})",
            R"({"schema_version":1,"session_id":"talk","policy_revision":1,"outputs":{"speech":true},"device_capabilities":[""]})"}) {
        auto* bad=cJSON_Parse(raw); SessionOutputPlan refused;
        assert(!ParseSessionOutputPlan(bad,refused)); cJSON_Delete(bad);
    }
    DeviceOutputGate settings_gate(false);
    assert(settings_gate.CapabilityBinding().session_id.empty());
    assert(settings_gate.Bind(DeviceOutputPolicy{true,1,speech,false,true}));
    with_settings.policy_revision=1;
    assert(settings_gate.Start(with_settings,"talk"));
    const auto binding=settings_gate.CapabilityBinding();
    assert(binding.session_id=="talk" && binding.policy_revision==1 && binding.capabilities.size()==2);
    settings_gate.Suspend();
    assert(settings_gate.CapabilityBinding().session_id.empty());  // a suspended session acts on nothing
    assert(settings_gate.Resume("talk"));
    settings_gate.Close();
    assert(settings_gate.CapabilityBinding().session_id.empty());

    // A device's tool restriction narrows the plan without a new policy
    // revision (the device is never told to refetch its policy for it): a plan
    // listing fewer capabilities under the revision it holds still opens, and
    // binds only what it lists, so the device refuses the rest as unplanned.
    SessionOutputPlan narrowed = with_settings;
    narrowed.session_id = "talk-2";
    narrowed.device_capabilities = {"audio.volume.adjust"};
    assert(settings_gate.Start(narrowed,"talk-2"));
    const auto narrowed_binding=settings_gate.CapabilityBinding();
    assert(narrowed_binding.policy_revision==1);
    assert((narrowed_binding.capabilities==std::vector<std::string>{"audio.volume.adjust"}));
    settings_gate.Close();
}
