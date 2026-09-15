#include "eidolon/output_policy.h"
#include <cassert>
#include <cJSON.h>
#include <fstream>
#include <sstream>
using namespace eidolon;
using presentation::Output;
int main() {
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
}
