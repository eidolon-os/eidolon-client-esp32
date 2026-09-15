#include "eidolon/expression/delivery.h"
#include <cassert>
#include <fstream>
#include <sstream>
#include <vector>
#include <cJSON.h>
using namespace eidolon::expression;
class FakeSurface: public Surface {
public:
    int submits=0;
    Runtime runtime;
    Observer observer;
    Event Submit(const Plan& plan) override { ++submits;return runtime.Submit(plan,0); }
    Event Cancel(uint64_t token) override { auto e=runtime.Cancel(token);if(observer)observer(e);return e; }
    void SetObserver(Observer cb) override { observer=std::move(cb); }
};
int main() {
    std::ifstream f("tests/fixtures/presentation/expression-plan.json");
    std::ostringstream stream;stream<<f.rdbuf();const auto wire=stream.str();
    FakeSurface surface;
    std::vector<std::string> receipts;
    Delivery delivery([&](auto id,auto receipt){ assert(id=="cmd-1");receipts.push_back(receipt); });
    surface.SetObserver([&](auto e){delivery.Observe(e,100);});
    assert(delivery.Play(wire,"cmd-1",surface,0,1));
    assert(surface.submits==1 && receipts.size()==1);
    assert(delivery.Play(wire,"cmd-1",surface,0,1));
    assert(surface.submits==1 && receipts.size()==2);
    assert(!delivery.Play(wire,"collision",surface,0,1));
    // Receiving and preparing never imply that a display has shown a frame.
    surface.runtime.Tick(500);assert(receipts.size()==2);
    auto started=surface.runtime.Presented();delivery.Observe(started,500);
    delivery.Observe(started,510);assert(receipts.size()==3);
    surface.runtime.Tick(1500);delivery.Observe(surface.runtime.Presented(),1500);
    auto* json=cJSON_Parse(receipts.back().c_str());
    assert(std::string(cJSON_GetObjectItem(json,"status")->valuestring)=="completed");
    assert(cJSON_GetObjectItem(json,"sequence")->valueint==3);cJSON_Delete(json);
    assert(delivery.Play(wire,"cmd-1",surface,1600,1));assert(surface.submits==1);
    delivery.Close(surface);
    delivery.Observe(started,2000);assert(receipts.size()==5);
    // A future offset must not emit started while only the base face is visible.
    Plan delayed;delayed.token=9;delayed.count=1;delayed.max_duration_ms=2000;
    delayed.steps[0].at_ms=1000;delayed.steps[0].duration_ms=1000;
    assert(surface.runtime.Submit(delayed,3000).status==Status::Accepted);
    surface.runtime.Tick(3100);assert(surface.runtime.Presented().status==Status::None);
    surface.runtime.Tick(4100);assert(surface.runtime.Presented().status==Status::Started);
}
