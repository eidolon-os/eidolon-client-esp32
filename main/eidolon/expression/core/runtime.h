#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "eidolon/expression/generated/presentation_catalog.h"

namespace eidolon::expression {

enum class BaseState : uint8_t { Idle, Listening, Thinking, Error };
enum class Status : uint8_t { None, Accepted, Started, Completed, Cancelled, Rejected, Failed };
enum class Reason : uint8_t { None, InvalidPlan, Busy, Superseded, Hidden, Cancelled, Expired };

struct FacePose {
    float gaze_x = 0, gaze_y = 0;  // -1..1
    float left_open = 1, right_open = 1;
    float left_tilt = 0, right_tilt = 0;  // signed degrees
    float eye_size = 0;  // -1..1
    float mouth_open = 0;
};
struct Step {
    Gesture gesture = Gesture::Attend;
    float intensity = .3f;
    uint32_t at_ms = 0, duration_ms = 1000;
    bool subtle = false;
};
struct Plan {
    // IDs and session authority are held by the transport adapter, not this core.
    uint64_t token = 0;
    uint32_t max_duration_ms = 0;
    size_t count = 0;
    std::array<Step, kMaxSteps> steps{};
};
struct Event {
    uint64_t token = 0;
    Status status = Status::None;
    Reason reason = Reason::None;
};

// Single-threaded by design. Network callbacks must enqueue to the UI owner.
// tick() prepares a pose; presented() must follow an actual render completion.
class Runtime {
public:
    static bool Valid(const Plan& plan);
    Event Submit(const Plan& plan, uint64_t now_ms);
    Event Cancel(uint64_t token, Reason reason = Reason::Cancelled);
    Event SetVisible(bool visible);
    void SetBase(BaseState state) { base_ = state; }
    FacePose Tick(uint64_t now_ms);
    Event Presented();
    bool active() const { return active_; }
    uint64_t token() const { return plan_.token; }
private:
    FacePose Base(uint64_t now_ms) const;
    Plan plan_{};
    BaseState base_ = BaseState::Idle;
    uint64_t started_ms_ = 0;
    bool frame_prepared_ = false;
    bool active_ = false, started_ = false, finished_frame_ = false, visible_ = true;
};
}  // namespace eidolon::expression
