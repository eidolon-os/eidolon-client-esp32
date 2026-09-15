#include "eidolon/expression/core/wire.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
using namespace eidolon::expression;
int main() {
    std::ifstream file("tests/fixtures/presentation/expression-plan.json");
    std::string json{std::istreambuf_iterator<char>(file),{}};
    WirePlan out;
    assert(DecodePlan(json,42,out));
    assert(out.plan.token==42 && out.plan.steps[0].gesture==Gesture::Affirm);
    assert(out.plan.steps[0].subtle && out.plan.steps[0].duration_ms==1200);
    assert(std::string(out.presentation_id.data())=="presentation-1");
    assert(!DecodePlan(json+"x",42,out));
    assert(!DecodePlan(std::string(2049,' '),42,out));
    for (auto replacement:{"\"schema_version\":true,", "\"script\":\"anything\",",
                           "\"profile\":\"unknown\",", "\"response_id\":\"duplicate\","}) {
        auto bad=json;bad.insert(1,replacement);assert(!DecodePlan(bad,42,out));
    }
    auto bad=json;auto pos=bad.find("1200");bad.replace(pos,4,"true");assert(!DecodePlan(bad,42,out));
    bad=json;pos=bad.find("affirm");bad.replace(pos,6,"unknown");assert(!DecodePlan(bad,42,out));
    bad=json;pos=bad.find("0.3");bad.replace(pos,3,"-1");assert(!DecodePlan(bad,42,out));
}
