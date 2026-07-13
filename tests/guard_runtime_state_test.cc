#include <cassert>
#include <cstdint>
#include <vector>

#include "eidolon/guard/guard_motion.h"
#include "eidolon/guard/guard_presence_adapter.h"
#include "eidolon/guard/guard_state_machine.h"

namespace {

constexpr uint32_t Fourcc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
}

eidolon::GuardRuntimeConfig TestConfig()
{
    eidolon::GuardRuntimeConfig config;
    config.sample_interval_ms = 100;
    config.motion_threshold = 18;
    config.motion_clear_threshold = 8;
    config.candidate_debounce_ms = 300;
    config.absence_timeout_ms = 1000;
    config.consecutive_capture_failures = 3;
    return config;
}

eidolon::GuardSample Sample(uint64_t now_ms, uint32_t score)
{
    return {
        .now_ms = now_ms,
        .frame_ok = true,
        .motion_valid = true,
        .motion_score = score,
    };
}

void TestDisabledStartStop()
{
    eidolon::GuardStateMachine machine;
    machine.ApplyConfig(TestConfig());

    auto observation = machine.ProcessSample(Sample(0, 255));
    assert(observation.state == eidolon::GuardState::Disabled);

    observation = machine.Start(10);
    assert(observation.state == eidolon::GuardState::Idle);
    assert(observation.epoch == 1);

    observation = machine.Stop(20);
    assert(observation.state == eidolon::GuardState::Disabled);
}

void TestCandidateDebounce()
{
    eidolon::GuardStateMachine machine;
    machine.ApplyConfig(TestConfig());
    machine.Start(0);

    auto observation = machine.ProcessSample(Sample(100, 22));
    assert(observation.state == eidolon::GuardState::CandidatePending);
    const uint32_t first_epoch = observation.epoch;

    observation = machine.ProcessSample(Sample(250, 22));
    assert(observation.state == eidolon::GuardState::CandidatePending);

    observation = machine.ProcessSample(Sample(399, 22));
    assert(observation.state == eidolon::GuardState::CandidatePending);

    observation = machine.ProcessSample(Sample(400, 22));
    assert(observation.state == eidolon::GuardState::Candidate);
    assert(observation.epoch == first_epoch);
}

void TestDebounceClears()
{
    eidolon::GuardStateMachine machine;
    machine.ApplyConfig(TestConfig());
    machine.Start(0);

    auto observation = machine.ProcessSample(Sample(100, 22));
    assert(observation.state == eidolon::GuardState::CandidatePending);

    observation = machine.ProcessSample(Sample(200, 2));
    assert(observation.state == eidolon::GuardState::Idle);
}

void TestAbsenceOnceAndReentryEpoch()
{
    eidolon::GuardStateMachine machine;
    machine.ApplyConfig(TestConfig());
    machine.Start(0);

    auto observation = machine.ProcessSample(Sample(100, 22));
    const uint32_t first_epoch = observation.epoch;
    observation = machine.ProcessSample(Sample(400, 22));
    assert(observation.state == eidolon::GuardState::Candidate);

    observation = machine.ProcessSample(Sample(500, 0));
    assert(observation.state == eidolon::GuardState::AbsentPending);
    const uint32_t absent_pending_sequence = observation.sequence;

    observation = machine.ProcessSample(Sample(1000, 0));
    assert(observation.state == eidolon::GuardState::AbsentPending);
    assert(observation.sequence == absent_pending_sequence);

    observation = machine.ProcessSample(Sample(1400, 0));
    assert(observation.state == eidolon::GuardState::Absent);
    const uint32_t absent_sequence = observation.sequence;

    observation = machine.ProcessSample(Sample(1600, 0));
    assert(observation.state == eidolon::GuardState::Absent);
    assert(observation.sequence == absent_sequence);

    observation = machine.ProcessSample(Sample(1700, 22));
    assert(observation.state == eidolon::GuardState::CandidatePending);
    assert(observation.epoch == first_epoch + 1);
}

void TestCaptureFailureFault()
{
    eidolon::GuardStateMachine machine;
    machine.ApplyConfig(TestConfig());
    machine.Start(0);

    eidolon::GuardSample failure = {
        .now_ms = 100,
        .frame_ok = false,
        .motion_valid = false,
        .motion_score = 0,
    };
    auto observation = machine.ProcessSample(failure);
    assert(observation.state == eidolon::GuardState::Idle);
    failure.now_ms = 200;
    observation = machine.ProcessSample(failure);
    assert(observation.state == eidolon::GuardState::Idle);
    failure.now_ms = 300;
    observation = machine.ProcessSample(failure);
    assert(observation.state == eidolon::GuardState::Fault);
    assert(observation.fault == eidolon::GuardFaultCode::CaptureFailures);
}

void TestLuminanceGridAndMotionScore()
{
    constexpr int width = 48;
    constexpr int height = 36;
    std::vector<uint8_t> dark(width * height * 2, 0);
    std::vector<uint8_t> bright(width * height * 2, 0);
    std::fill(dark.begin(), dark.begin() + width * height, 10);
    std::fill(bright.begin(), bright.begin() + width * height, 40);

    CameraFrame dark_frame = {
        .data = dark.data(),
        .len = dark.size(),
        .width = width,
        .height = height,
        .pixel_format = Fourcc('4', '2', '2', 'P'),
    };
    CameraFrame bright_frame = dark_frame;
    bright_frame.data = bright.data();

    eidolon::GuardLuminanceGrid before = {};
    eidolon::GuardLuminanceGrid after = {};
    assert(eidolon::ReadGuardLuminanceGrid(dark_frame, before));
    assert(eidolon::ReadGuardLuminanceGrid(bright_frame, after));
    assert(eidolon::GuardMotionScore(before, after) == 30);
}

void TestRgb565LuminanceGrid()
{
    constexpr int width = 48;
    constexpr int height = 36;
    std::vector<uint8_t> red(width * height * 2, 0);
    std::vector<uint8_t> green(width * height * 2, 0);
    for (int i = 0; i < width * height; ++i) {
        const uint16_t red565 = 0xf800;
        const uint16_t green565 = 0x07e0;
        red[i * 2] = static_cast<uint8_t>(red565 & 0xff);
        red[i * 2 + 1] = static_cast<uint8_t>(red565 >> 8);
        green[i * 2] = static_cast<uint8_t>(green565 & 0xff);
        green[i * 2 + 1] = static_cast<uint8_t>(green565 >> 8);
    }

    CameraFrame red_frame = {
        .data = red.data(),
        .len = red.size(),
        .width = width,
        .height = height,
        .pixel_format = Fourcc('R', 'G', 'B', 'P'),
    };
    CameraFrame green_frame = red_frame;
    green_frame.data = green.data();

    eidolon::GuardLuminanceGrid before = {};
    eidolon::GuardLuminanceGrid after = {};
    assert(eidolon::ReadGuardLuminanceGrid(red_frame, before));
    assert(eidolon::ReadGuardLuminanceGrid(green_frame, after));
    assert(before[0] == 76);
    assert(after[0] == 149);
    assert(eidolon::GuardMotionScore(before, after) == 73);
}

eidolon::GuardPresenceRuntime PresenceRuntime()
{
    return {
        .guard_companion_id = "guard-test",
        .device_id = "atk-test",
        .runtime_revision = 7,
        .candidate_debounce_ms = 300,
        .boot_nonce = 0x1a2b3c4d,
    };
}

eidolon::GuardObservation Observation(eidolon::GuardState state, uint32_t epoch, uint32_t sequence,
                                      uint64_t now_ms, uint64_t last_seen_ms, uint32_t score)
{
    return {
        .epoch = epoch,
        .sequence = sequence,
        .state = state,
        .motion_score = score,
        .now_ms = now_ms,
        .last_seen_ms = last_seen_ms,
    };
}

void TestPresenceAdapterEmitsOnlyTransitionFacts()
{
    eidolon::GuardPresenceAdapter adapter;
    adapter.Configure(PresenceRuntime());

    assert(!adapter.Build(Observation(eidolon::GuardState::Idle, 1, 1, 100, 0, 0), 10'000));
    const auto candidate = adapter.Build(
        Observation(eidolon::GuardState::Candidate, 4, 8, 1400, 1200, 37), 1'700'000'000'000ULL);
    assert(candidate.has_value());
    assert(candidate->find("\"type\":\"guard.presence.candidate\"") != std::string::npos);
    assert(candidate->find("\"correlation_id\":\"g-1a2b3c4d-r7-e4\"") != std::string::npos);
    assert(candidate->find("\"runtime_revision\":7") != std::string::npos);
    assert(candidate->find("\"motion_score\":37") != std::string::npos);
    assert(candidate->find("\"raw_retention\":\"none\"") != std::string::npos);
    assert(!adapter.Build(Observation(eidolon::GuardState::Candidate, 4, 9, 1500, 1200, 38), 10'001));

    const auto absent =
        adapter.Build(Observation(eidolon::GuardState::Absent, 4, 11, 3500, 1200, 0), 10'002);
    assert(absent.has_value());
    assert(absent->find("\"type\":\"guard.presence.absent\"") != std::string::npos);
    assert(absent->find("\"absent_for_ms\":2300") != std::string::npos);
    assert(absent->find("\"signals\"") == std::string::npos);
    assert(absent->find("\"camera\"") == std::string::npos);
    assert(absent->find("\"motion_score\"") == std::string::npos);
    assert(!adapter.Build(Observation(eidolon::GuardState::Absent, 4, 12, 3600, 1200, 0), 10'003));
}

void TestPresenceAdapterEscapesSignedRuntimeIdentity()
{
    eidolon::GuardPresenceAdapter adapter;
    auto runtime = PresenceRuntime();
    runtime.guard_companion_id = "guard-\\\"safe";
    runtime.device_id = "atk\nunit";
    adapter.Configure(runtime);

    const auto candidate =
        adapter.Build(Observation(eidolon::GuardState::Candidate, 1, 1, 100, 100, 1), 100);
    assert(candidate.has_value());
    const std::string escaped_identity = "guard-" + std::string(3, '\\') + "\"safe";
    assert(candidate->find(escaped_identity) != std::string::npos);
    assert(candidate->find("atk\\nunit") != std::string::npos);
}

}  // namespace

int main()
{
    TestDisabledStartStop();
    TestCandidateDebounce();
    TestDebounceClears();
    TestAbsenceOnceAndReentryEpoch();
    TestCaptureFailureFault();
    TestLuminanceGridAndMotionScore();
    TestRgb565LuminanceGrid();
    TestPresenceAdapterEmitsOnlyTransitionFacts();
    TestPresenceAdapterEscapesSignedRuntimeIdentity();
    return 0;
}
