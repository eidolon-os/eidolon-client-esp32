#include "ambient_presence_state.h"
#include "guard/owner_presence_state_machine.h"

#include <cassert>
#include <iostream>

namespace {

eidolon::OwnerPresenceSample Face(uint64_t now_ms, bool match)
{
    return {
        .now_ms = now_ms,
        .profile_active = true,
        .profile_revision = 7,
        .face_evaluated = true,
        .face_match = match,
    };
}

eidolon::OwnerPresenceSample Person(uint64_t now_ms, bool present)
{
    return {
        .now_ms = now_ms,
        .profile_active = true,
        .profile_revision = 7,
        .person_evaluated = true,
        .person_present = present,
    };
}

void TestPersistentArmingAndOwnerReappearance()
{
    eidolon::AmbientPresenceRegistry ambient;
    eidolon::OwnerPresenceStateMachine owner;
    owner.ApplyConfig({
        .enter_ms = 600,
        .exit_ms = 12000,
        .heartbeat_ms = 10000,
        .lease_ms = 30000,
    });
    owner.Process(Face(0, false));

    const eidolon::AmbientPresenceState edge = {
        .present = true,
        .presence_epoch = 1,
        .sequence = 1,
        .lease_ms = 15000,
        .observation = eidolon::AmbientPresenceObservation::Edge,
    };
    assert(ambient.Apply("box", "presence-flow", "radar-edge", edge,
                         1000, 1000) ==
           eidolon::AmbientPresenceApplyResult::Armed);

    // No bounded camera request exists. A heartbeat keeps the generic
    // assertion alive while the camera has no face.
    auto heartbeat = edge;
    heartbeat.sequence = 2;
    heartbeat.observation = eidolon::AmbientPresenceObservation::Heartbeat;
    assert(ambient.Apply("box", "presence-flow", "radar-heartbeat",
                         heartbeat, 14000, 14000) ==
           eidolon::AmbientPresenceApplyResult::Refreshed);

    // The owner can arrive arbitrarily later and immediately consumes the
    // still-active assertion.
    auto observation = owner.Process(Face(15000, true));
    assert(observation.state == eidolon::OwnerPresenceState::PresentPending);
    observation = owner.Process(Face(15600, true));
    assert(observation.fact == eidolon::OwnerPresenceFact::Present);
    auto pending = ambient.PendingOwnerConfirmations();
    assert(pending.size() == 1);
    assert(pending[0].root_event_id == "radar-edge");
    assert(ambient.MarkOwnerConfirmed("box", 1));
    const uint32_t first_owner_epoch = observation.epoch;

    // A transient NO FACE shorter than exit_ms preserves owner identity and
    // does not synthesize another authentication.
    observation = owner.Process(Person(16000, false));
    assert(observation.state == eidolon::OwnerPresenceState::AbsentPending);
    observation = owner.Process(Person(20000, true));
    assert(observation.state == eidolon::OwnerPresenceState::Present);
    assert(observation.epoch == first_owner_epoch);
    assert(ambient.PendingOwnerConfirmations().empty());

    // A sustained departure closes the owner epoch but deliberately leaves the
    // radar assertion armed.
    owner.Process(Person(21000, false));
    heartbeat.sequence = 3;
    assert(ambient.Apply("box", "presence-flow", "radar-heartbeat-2",
                         heartbeat, 30000, 30000) ==
           eidolon::AmbientPresenceApplyResult::Refreshed);
    observation = owner.Process(Person(33100, false));
    assert(observation.fact == eidolon::OwnerPresenceFact::Absent);
    ambient.OnOwnerAbsent();
    assert(ambient.HasActive());
    assert(ambient.PendingOwnerConfirmations().size() == 1);

    // Owner return works without VACANT -> PRESENT and without a retry timer.
    observation = owner.Process(Face(35000, true));
    assert(observation.state == eidolon::OwnerPresenceState::PresentPending);
    observation = owner.Process(Face(35600, true));
    assert(observation.fact == eidolon::OwnerPresenceFact::Present);
    assert(observation.epoch == first_owner_epoch + 1);
    assert(ambient.PendingOwnerConfirmations().size() == 1);

    const eidolon::AmbientPresenceState vacant = {
        .present = false,
        .presence_epoch = 1,
        .sequence = 4,
        .lease_ms = 0,
        .observation = eidolon::AmbientPresenceObservation::Edge,
    };
    assert(ambient.Apply("box", "presence-flow", "radar-vacant",
                         vacant, 36000, 36000) ==
           eidolon::AmbientPresenceApplyResult::Disarmed);
    assert(!ambient.HasActive());
}

}  // namespace

int main()
{
    TestPersistentArmingAndOwnerReappearance();
    std::cout << "presence_owner_orchestration_test: PASS\n";
    return 0;
}
