#include "presence_wake_flow.h"

#include <cassert>
#include <iostream>

int main()
{
    eidolon::PresenceWakeFlowTracker tracker({
        .flow_timeout_ms = 3000,
        .cooldown_ms = 30000,
    });
    assert(tracker.Start("", "event-1", 1000) ==
           eidolon::PresenceWakeStartResult::Ignored);
    assert(tracker.Start("flow-1", "event-1", 1000) ==
           eidolon::PresenceWakeStartResult::Started);
    assert(tracker.waiting());
    assert(tracker.pending_flow_id() == "flow-1");
    assert(tracker.deadline_ms() == 4000);
    assert(tracker.Start("flow-2", "event-2", 1100) ==
           eidolon::PresenceWakeStartResult::Ignored);
    assert(tracker.Confirm("other-flow", 1200) ==
           eidolon::PresenceWakeConfirmationResult::Ignored);
    assert(tracker.Confirm("flow-1", 4000) ==
           eidolon::PresenceWakeConfirmationResult::Matched);
    assert(!tracker.waiting());

    assert(tracker.Start("flow-cooldown", "event-cooldown", 30999) ==
           eidolon::PresenceWakeStartResult::Ignored);
    assert(tracker.Start("flow-2", "event-2", 31000) ==
           eidolon::PresenceWakeStartResult::Started);
    assert(!tracker.Expire(33999));
    assert(tracker.Expire(34000));
    assert(!tracker.waiting());

    assert(tracker.Start("flow-3", "event-3", 61000) ==
           eidolon::PresenceWakeStartResult::Started);
    assert(tracker.Cancel());
    assert(!tracker.Cancel());

    std::cout << "presence_wake_flow_test: PASS\n";
    return 0;
}
