#include "ambient_presence_state.h"

#include <cassert>
#include <iostream>

int main()
{
    eidolon::AmbientPresenceActivationGate activation_gate;
    assert(activation_gate.TryConsume(1, "atk-1", 10000, 12, 33));
    assert(!activation_gate.TryConsume(1, "atk-1", 10001, 12, 34));
    assert(activation_gate.ApplyOwnerLifecycle(
               "other-atk", false, 11000, 12, 34) ==
           eidolon::AmbientOwnerLifecycleApplyResult::Rejected);
    assert(activation_gate.ApplyOwnerLifecycle(
               "atk-1", true, 9000, 12, 34) ==
           eidolon::AmbientOwnerLifecycleApplyResult::Stale);
    // A newer signed fact can establish a restarted Guard incarnation even
    // when its in-memory epoch/sequence normalized.
    assert(activation_gate.ApplyOwnerLifecycle(
               "atk-1", true, 11000, 0, 1) ==
           eidolon::AmbientOwnerLifecycleApplyResult::Present);
    assert(activation_gate.ApplyOwnerLifecycle(
               "atk-1", false, 12000, 0, 2) ==
           eidolon::AmbientOwnerLifecycleApplyResult::Absent);
    assert(activation_gate.TryConsume(1, "atk-1", 13000, 0, 3));
    activation_gate.ResetForRadarTransition();
    assert(activation_gate.TryConsume(2, "atk-1", 14000, 0, 4));

    eidolon::AmbientPresenceRegistry registry;
    const eidolon::AmbientPresenceState edge = {
        .present = true,
        .presence_epoch = 1,
        .sequence = 1,
        .lease_ms = 15000,
        .observation = eidolon::AmbientPresenceObservation::Edge,
    };
    assert(registry.Apply("box-1", "flow-1", "radar-1", edge, 1000, 1000) ==
           eidolon::AmbientPresenceApplyResult::Armed);
    assert(registry.HasActive());
    auto pending = registry.PendingOwnerConfirmations();
    assert(pending.size() == 1);
    assert(pending[0].root_event_id == "radar-1");

    assert(registry.MarkOwnerConfirmed("box-1", 1));
    assert(registry.PendingOwnerConfirmations().empty());

    // A state-repair heartbeat renews the assertion but never behaves like a
    // new trigger or reopens an already consumed owner confirmation.
    auto heartbeat = edge;
    heartbeat.sequence = 2;
    heartbeat.observation = eidolon::AmbientPresenceObservation::Heartbeat;
    assert(registry.Apply("box-1", "flow-1", "heartbeat-1", heartbeat,
                          6000, 6000) ==
           eidolon::AmbientPresenceApplyResult::Refreshed);
    assert(registry.PendingOwnerConfirmations().empty());

    // An owner departure reopens every still-active radar assertion. The next
    // owner appearance does not require another radar edge.
    registry.OnOwnerAbsent();
    pending = registry.PendingOwnerConfirmations();
    assert(pending.size() == 1);
    assert(pending[0].flow_id == "flow-1");

    // Reordered state cannot overwrite a newer heartbeat.
    assert(registry.Apply("box-1", "flow-1", "stale", edge, 7000, 7000) ==
           eidolon::AmbientPresenceApplyResult::Stale);

    const eidolon::AmbientPresenceState vacant = {
        .present = false,
        .presence_epoch = 1,
        .sequence = 3,
        .lease_ms = 0,
        .observation = eidolon::AmbientPresenceObservation::Edge,
    };
    assert(registry.Apply("box-1", "flow-1", "vacant-1", vacant,
                          8000, 8000) ==
           eidolon::AmbientPresenceApplyResult::Disarmed);
    assert(!registry.HasActive());

    auto reentry = edge;
    reentry.presence_epoch = 2;
    reentry.sequence = 4;
    assert(registry.Apply("box-1", "flow-2", "radar-2", reentry,
                          9000, 9000) ==
           eidolon::AmbientPresenceApplyResult::Armed);
    assert(registry.Expire(23999).empty());
    auto expired = registry.Expire(24000);
    assert(expired.size() == 1);
    assert(expired[0].flow_id == "flow-2");
    assert(!registry.HasActive());

    // A consumed epoch remains consumed across lease expiry and a later
    // snapshot. This prevents a normal Voice Room end from immediately
    // authenticating and reopening while the same owner never left.
    assert(!registry.MarkOwnerConfirmed("box-1", 2));
    auto repair_before_confirm = reentry;
    repair_before_confirm.sequence = 5;
    repair_before_confirm.observation =
        eidolon::AmbientPresenceObservation::Snapshot;
    assert(registry.Apply("box-1", "flow-2", "repair-before-confirm",
                          repair_before_confirm, 25000, 25000) ==
           eidolon::AmbientPresenceApplyResult::Armed);
    assert(registry.MarkOwnerConfirmed("box-1", 2));
    assert(registry.Expire(40000).size() == 1);
    auto repair = reentry;
    repair.sequence = 6;
    repair.observation = eidolon::AmbientPresenceObservation::Snapshot;
    assert(registry.Apply("box-1", "flow-2", "snapshot-2", repair,
                          41000, 41000) ==
           eidolon::AmbientPresenceApplyResult::Armed);
    assert(registry.PendingOwnerConfirmations().empty());

    // Network loss deactivates assertions without erasing the consumed epoch.
    registry.DeactivateAll();
    assert(!registry.HasActive());
    repair.sequence = 7;
    assert(registry.Apply("box-1", "flow-2", "snapshot-after-network",
                          repair, 42000, 42000) ==
           eidolon::AmbientPresenceApplyResult::Armed);
    assert(registry.PendingOwnerConfirmations().empty());

    // A real local owner departure reopens the same physical presence epoch,
    // including when the assertion was repaired after expiry.
    registry.OnOwnerAbsent();
    assert(registry.PendingOwnerConfirmations().size() == 1);

    // A publisher reboot can reset its in-memory epoch/sequence. A newer fact
    // on a different flow establishes the new incarnation; delayed packets
    // from the old flow cannot resurrect the previous higher epoch.
    eidolon::AmbientPresenceRegistry reboot_registry;
    auto before_reboot = edge;
    before_reboot.presence_epoch = 9;
    before_reboot.sequence = 80;
    assert(reboot_registry.Apply("box-reboot", "old-flow", "old-event",
                                 before_reboot, 10000, 1000) ==
           eidolon::AmbientPresenceApplyResult::Armed);
    auto after_reboot = edge;
    after_reboot.presence_epoch = 1;
    after_reboot.sequence = 1;
    assert(reboot_registry.Apply("box-reboot", "new-flow", "new-event",
                                 after_reboot, 20000, 2000) ==
           eidolon::AmbientPresenceApplyResult::Armed);
    before_reboot.sequence = 81;
    before_reboot.observation =
        eidolon::AmbientPresenceObservation::Heartbeat;
    assert(reboot_registry.Apply("box-reboot", "old-flow", "delayed-old",
                                 before_reboot, 15000, 3000) ==
           eidolon::AmbientPresenceApplyResult::Stale);
    pending = reboot_registry.PendingOwnerConfirmations();
    assert(pending.size() == 1);
    assert(pending[0].flow_id == "new-flow");
    assert(pending[0].presence_epoch == 1);

    std::cout << "ambient_presence_state_test: PASS\n";
    return 0;
}
