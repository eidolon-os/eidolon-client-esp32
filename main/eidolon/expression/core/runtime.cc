#include "runtime.h"
#include <algorithm>
#include <cmath>

namespace eidolon::expression {
namespace {
float Mix(float a, float b, float t) { return a + (b-a)*t; }
FacePose Blend(const FacePose& a, const FacePose& b, float t) {
    return {Mix(a.gaze_x,b.gaze_x,t), Mix(a.gaze_y,b.gaze_y,t),
            Mix(a.left_open,b.left_open,t), Mix(a.right_open,b.right_open,t),
            Mix(a.left_tilt,b.left_tilt,t), Mix(a.right_tilt,b.right_tilt,t),
            Mix(a.eye_size,b.eye_size,t), Mix(a.mouth_open,b.mouth_open,t)};
}
// This is the only visual recipe catalog. Transport consumers send gesture IDs.
FacePose Pose(Gesture gesture) {
    FacePose p;
    switch (gesture) {
    case Gesture::Attend: p.eye_size=.35f; break;
    case Gesture::Affirm: p.left_open=p.right_open=.35f; p.left_tilt=15; p.right_tilt=-15; break;
    case Gesture::Ponder: p.gaze_x=.7f; p.gaze_y=-.25f; p.left_open=p.right_open=.75f; break;
    case Gesture::Question: p.left_open=.6f; p.right_open=1; p.left_tilt=-20; p.eye_size=.2f; break;
    case Gesture::Soften: p.left_open=p.right_open=.65f; p.left_tilt=-12; p.right_tilt=12; break;
    case Gesture::Delight: p.left_open=p.right_open=.6f; p.left_tilt=25; p.right_tilt=-25; p.mouth_open=.5f; break;
    case Gesture::Hesitate: p.gaze_x=-.45f; p.left_open=p.right_open=.7f; break;
    case Gesture::Attention: p.eye_size=.7f; p.mouth_open=.35f; break;
    }
    return p;
}
}  // namespace
bool Runtime::Valid(const Plan& plan) {
    if (!plan.token || !plan.count || plan.count > kMaxSteps ||
        plan.max_duration_ms > kMaxDurationMs) return false;
    uint32_t end=0;
    for (size_t i=0;i<plan.count;++i) {
        const auto& s=plan.steps[i];
        if (static_cast<unsigned>(s.gesture)>static_cast<unsigned>(Gesture::Attention) ||
            !std::isfinite(s.intensity) || s.intensity<0 || s.intensity>1 ||
            s.at_ms<end || s.at_ms>kMaxDurationMs || s.duration_ms<150 ||
            s.duration_ms>kMaxDurationMs) return false;
        end=s.at_ms+s.duration_ms;
        if (end>plan.max_duration_ms) return false;
    }
    return true;
}
Event Runtime::Submit(const Plan& plan,uint64_t now_ms) {
    if (!Valid(plan)) return {plan.token,Status::Rejected,Reason::InvalidPlan};
    if (!visible_) return {plan.token,Status::Rejected,Reason::Hidden};
    if (active_) return {plan.token,Status::Rejected,Reason::Busy};
    plan_=plan;started_ms_=now_ms;active_=true;started_=false;finished_frame_=false;frame_prepared_=false;
    return {plan.token,Status::Accepted,Reason::None};
}
Event Runtime::Cancel(uint64_t token,Reason reason) {
    if (!active_ || plan_.token!=token) return {};
    active_=false;finished_frame_=false;
    return {token,Status::Cancelled,reason};
}
Event Runtime::SetVisible(bool visible) {
    visible_=visible;
    return visible ? Event{} : Cancel(plan_.token,Reason::Hidden);
}
FacePose Runtime::Base(uint64_t now_ms) const {
    FacePose p;
    if (base_==BaseState::Listening) p.eye_size=.12f;
    if (base_==BaseState::Thinking) { p.gaze_x=.25f;p.left_open=p.right_open=.8f; }
    if (base_==BaseState::Error) { p.left_open=.65f;p.right_open=.9f;p.left_tilt=-15; }
    // Bounded periodic blink; no per-frame allocation or global random state.
    const auto phase=now_ms%4300;
    if (phase<160) {
        const float open=std::abs(static_cast<float>(phase)-80)/80;
        p.left_open*=open;p.right_open*=open;
    }
    return p;
}
FacePose Runtime::Tick(uint64_t now_ms) {
    const auto base=Base(now_ms);
    if (!active_) return base;
    frame_prepared_=false;
    const auto elapsed=now_ms>=started_ms_ ? now_ms-started_ms_ : 0;
    const auto& last=plan_.steps[plan_.count-1];
    if (elapsed>=last.at_ms+last.duration_ms) { finished_frame_=true;frame_prepared_=true;return base; }
    for (size_t i=0;i<plan_.count;++i) {
        const auto& s=plan_.steps[i];
        if (elapsed<s.at_ms || elapsed>=s.at_ms+s.duration_ms) continue;
        frame_prepared_=true;
        const float progress=static_cast<float>(elapsed-s.at_ms)/s.duration_ms;
        const float ramp=std::min({1.f,progress*5,(1-progress)*5});
        const float smooth=ramp*ramp*(3-2*ramp);
        // Baseline opens eyes during a gesture; automatic blinking cannot
        // overwrite an affirmation/wink and invent a second expression.
        FacePose stable=base;stable.left_open=stable.right_open=1;
        const float amplitude=(.5f+.5f*s.intensity)*(s.subtle?.7f:1.f);
        return Blend(stable,Pose(s.gesture),smooth*amplitude);
    }
    return base;
}
Event Runtime::Presented() {
    if (!active_ || !visible_ || !frame_prepared_) return {};
    frame_prepared_=false;
    if (finished_frame_ && !started_) { active_=false;return {plan_.token,Status::Failed,Reason::Expired}; }
    if (!started_) { started_=true;return {plan_.token,Status::Started,Reason::None}; }
    if (finished_frame_) { active_=false;return {plan_.token,Status::Completed,Reason::None}; }
    return {};
}
}  // namespace eidolon::expression
