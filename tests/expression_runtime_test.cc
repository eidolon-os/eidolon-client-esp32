#include "eidolon/expression/core/runtime.h"
#include <cassert>
#include <cmath>
#include <limits>
using namespace eidolon::expression;
int main() {
    Plan p; p.token=1;p.count=1;p.max_duration_ms=1200;
    p.steps[0]={Gesture::Affirm,.5f,0,1200,false};
    Runtime r;
    assert(r.Submit(p,1000).status==Status::Accepted);
    assert(r.Submit(p,1001).reason==Reason::Busy);
    assert(r.active());
    auto pose=r.Tick(1600);assert(pose.left_open<.7f);
    assert(r.Presented().status==Status::Started);
    r.SetBase(BaseState::Listening);pose=r.Tick(2300);
    assert(pose.eye_size>.1f && r.active()); // tick alone cannot complete
    assert(r.Presented().status==Status::Completed);
    assert(r.Presented().status==Status::None);
    p.token=2;assert(r.Submit(p,3000).status==Status::Accepted);
    assert(r.Cancel(1).status==Status::None); // stale cancel
    assert(r.SetVisible(false).status==Status::Cancelled);
    assert(r.Submit(p,4000).reason==Reason::Hidden);
    r.SetVisible(true);p.steps[0].intensity=std::numeric_limits<float>::quiet_NaN();
    assert(r.Submit(p,4000).reason==Reason::InvalidPlan);
    p.steps[0].intensity=.5f;p.count=2;p.steps[1]={Gesture::Attend,.5f,500,500,false};
    assert(!Runtime::Valid(p));
    p.count=9;assert(!Runtime::Valid(p));
    p.count=1;p.steps[0].duration_ms=UINT32_MAX;assert(!Runtime::Valid(p));
    p.steps[0].duration_ms=1200;
    Runtime late;
    late.Submit(p,1000);
    assert(late.Presented().status==Status::None); // old frame after a new submit
    late.Tick(3000);
    assert(late.Presented().status==Status::Failed); // missed entire gesture
    assert(!late.active());
    Runtime once;once.Submit(p,1000);once.Tick(1400);
    assert(once.Presented().status==Status::Started);
    assert(once.Presented().status==Status::None); // no new frame
    Runtime a,b;p.steps[0].duration_ms=1200;a.Submit(p,5000);b.Submit(p,5000);
    for (uint64_t t=5000;t<6200;t+=17) {auto x=a.Tick(t),y=b.Tick(t);assert(x.left_open==y.left_open && x.gaze_x==y.gaze_x);}
}
