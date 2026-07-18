#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "eidolon/guard/guard_motion.h"
#include "eidolon/guard/guard_presence_adapter.h"
#include "eidolon/guard/guard_state_machine.h"
#include "eidolon/guard/owner_presence_adapter.h"
#include "eidolon/guard/owner_presence_state_machine.h"

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

eidolon::OwnerPresenceSample OwnerSample(uint64_t now_ms, bool match,
                                         uint32_t revision = 5)
{
    return {
        .now_ms = now_ms,
        .profile_active = true,
        .profile_revision = revision,
        .face_evaluated = true,
        .face_match = match,
    };
}

eidolon::OwnerPresenceSample PersonSample(uint64_t now_ms, bool present,
                                          uint32_t revision = 5)
{
    return {
        .now_ms = now_ms,
        .profile_active = true,
        .profile_revision = revision,
        .person_evaluated = true,
        .person_present = present,
    };
}

void TestOwnerPresenceHasSmoothEnterExitAndHeartbeat()
{
    eidolon::OwnerPresenceStateMachine machine;
    machine.ApplyConfig({
        .enter_ms = 2000,
        .exit_ms = 6000,
        .heartbeat_ms = 4000,
        .lease_ms = 12000,
    });

    auto observation = machine.Process(OwnerSample(0, false));
    assert(observation.state == eidolon::OwnerPresenceState::Watching);
    observation = machine.Process(OwnerSample(1000, true));
    assert(observation.state == eidolon::OwnerPresenceState::PresentPending);
    assert(observation.fact == eidolon::OwnerPresenceFact::None);
    observation = machine.Process(PersonSample(2500, true));
    assert(observation.fact == eidolon::OwnerPresenceFact::None);
    observation = machine.Process(PersonSample(3000, true));
    assert(observation.state == eidolon::OwnerPresenceState::Present);
    assert(observation.fact == eidolon::OwnerPresenceFact::Present);
    const uint32_t epoch = observation.epoch;

    observation = machine.Process(PersonSample(4500, true));
    assert(observation.state == eidolon::OwnerPresenceState::Present);
    assert(observation.fact == eidolon::OwnerPresenceFact::None);
    observation = machine.Process(PersonSample(7000, true));
    assert(observation.fact == eidolon::OwnerPresenceFact::Present);
    assert(observation.epoch == epoch);

    observation = machine.Process(PersonSample(8000, false));
    assert(observation.state == eidolon::OwnerPresenceState::AbsentPending);
    assert(observation.identity_session_active);

    // An empty frame does not immediately declare the owner absent.
    observation = machine.Process(PersonSample(12999, false));
    assert(observation.fact == eidolon::OwnerPresenceFact::None);
    assert(observation.state == eidolon::OwnerPresenceState::AbsentPending);
    observation = machine.Process(PersonSample(13000, false));
    assert(observation.state == eidolon::OwnerPresenceState::Watching);
    assert(observation.fact == eidolon::OwnerPresenceFact::Absent);
    assert(observation.epoch == epoch);

    // Re-entry is face-gated and opens a new epoch.
    observation = machine.Process(OwnerSample(14000, true));
    assert(observation.state == eidolon::OwnerPresenceState::PresentPending);
    observation = machine.Process(OwnerSample(16000, true));
    assert(observation.state == eidolon::OwnerPresenceState::Present);
    assert(observation.fact == eidolon::OwnerPresenceFact::Present);
    assert(observation.epoch == epoch + 1);
}

void TestOwnerPresenceNonMatchDoesNotRevokeIdentitySession()
{
    eidolon::OwnerPresenceStateMachine machine;
    machine.ApplyConfig({
        .enter_ms = 1000,
        .exit_ms = 4000,
        .heartbeat_ms = 3000,
        .lease_ms = 10000,
    });

    auto observation = machine.Process(OwnerSample(0, false));
    assert(observation.state == eidolon::OwnerPresenceState::Watching);
    observation = machine.Process(OwnerSample(1000, true));
    observation = machine.Process(OwnerSample(2000, true));
    assert(observation.state == eidolon::OwnerPresenceState::Present);
    assert(observation.identity_session_active);
    const uint32_t epoch = observation.epoch;

    observation = machine.Process(OwnerSample(2500, false));
    assert(observation.state == eidolon::OwnerPresenceState::Present);
    assert(observation.identity_session_active);

    // The enrolled database only proves owner matches. A below-threshold face
    // result is not negative identity evidence, so person continuity may renew.
    observation = machine.Process(PersonSample(5000, true));
    assert(observation.state == eidolon::OwnerPresenceState::Present);
    assert(observation.identity_session_active);
    observation = machine.Process(PersonSample(6500, false));
    assert(observation.state == eidolon::OwnerPresenceState::AbsentPending);
    assert(observation.fact == eidolon::OwnerPresenceFact::None);
    observation = machine.Process(PersonSample(9000, false));
    assert(observation.state == eidolon::OwnerPresenceState::Watching);
    assert(observation.fact == eidolon::OwnerPresenceFact::Absent);
    assert(observation.epoch == epoch);
}

void TestOwnerPresenceRemainsPresentForStaticPerson()
{
    eidolon::OwnerPresenceStateMachine machine;
    machine.ApplyConfig({
        .enter_ms = 1000,
        .exit_ms = 12000,
        .heartbeat_ms = 10000,
        .lease_ms = 30000,
    });

    machine.Process(OwnerSample(0, false));
    auto observation = machine.Process(OwnerSample(500, true));
    assert(observation.state == eidolon::OwnerPresenceState::PresentPending);
    observation = machine.Process(PersonSample(1500, true));
    assert(observation.fact == eidolon::OwnerPresenceFact::Present);
    const uint32_t epoch = observation.epoch;

    for (uint64_t now_ms = 2000; now_ms <= 62000; now_ms += 500) {
        observation = machine.Process(PersonSample(now_ms, true));
        assert(observation.state == eidolon::OwnerPresenceState::Present);
        assert(observation.fact != eidolon::OwnerPresenceFact::Absent);
        assert(observation.identity_session_active);
        assert(observation.epoch == epoch);
    }
}

void TestOwnerPresenceProfileChangeClosesOldEpoch()
{
    eidolon::OwnerPresenceStateMachine machine;
    machine.ApplyConfig({.enter_ms = 500, .exit_ms = 2000, .heartbeat_ms = 1000,
                         .lease_ms = 5000});
    machine.Process(OwnerSample(0, false));
    machine.Process(OwnerSample(100, true));
    auto observation = machine.Process(OwnerSample(600, true));
    assert(observation.fact == eidolon::OwnerPresenceFact::Present);

    observation = machine.Process(OwnerSample(700, false, 6));
    assert(observation.state == eidolon::OwnerPresenceState::Watching);
    assert(observation.fact == eidolon::OwnerPresenceFact::Absent);
    assert(observation.fact_profile_revision == 5);
    assert(observation.profile_revision == 6);
}

void TestOwnerPresenceAdapterContainsOnlyMinimalFact()
{
    eidolon::OwnerPresenceStateMachine machine;
    machine.ApplyConfig({.enter_ms = 500, .exit_ms = 2000, .heartbeat_ms = 1000,
                         .lease_ms = 5000});
    machine.Process(OwnerSample(0, false));
    machine.Process(OwnerSample(100, true));
    const auto observation = machine.Process(OwnerSample(600, true));

    eidolon::OwnerPresenceAdapter adapter;
    adapter.Configure({
        .guard_companion_id = "guard-test",
        .device_id = "atk-test",
        .boot_nonce = 0x1234abcd,
    });
    const auto payload = adapter.Build(observation, 1'700'000'000'000ULL);
    assert(payload.has_value());
    assert(payload->find("\"type\":\"guard.owner_presence\"") != std::string::npos);
    assert(payload->find("\"state\":\"present\"") != std::string::npos);
    assert(payload->find("\"profile_revision\":5") != std::string::npos);
    assert(payload->find("\"lease_ms\":5000") != std::string::npos);
    assert(payload->find("op-1234abcd-r5-e1") != std::string::npos);
    assert(payload->find("similarity") == std::string::npos);
    assert(payload->find("faces") == std::string::npos);
    assert(payload->find("owner_id") == std::string::npos);
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
    TestOwnerPresenceHasSmoothEnterExitAndHeartbeat();
    TestOwnerPresenceNonMatchDoesNotRevokeIdentitySession();
    TestOwnerPresenceRemainsPresentForStaticPerson();
    TestOwnerPresenceProfileChangeClosesOldEpoch();
    TestOwnerPresenceAdapterContainsOnlyMinimalFact();
    return 0;
}
