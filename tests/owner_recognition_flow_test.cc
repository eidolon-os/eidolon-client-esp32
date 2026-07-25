#include "guard/owner_recognition_flow.h"

#include <cassert>
#include <iostream>

using eidolon::OwnerRecognitionFlowTracker;
using eidolon::OwnerRecognitionRequestResult;

int main()
{
    OwnerRecognitionFlowTracker tracker;

    assert(tracker.Request("", "event-1", 1000, false, 0) ==
           OwnerRecognitionRequestResult::Rejected);
    assert(tracker.Request("flow-1", "", 1000, false, 0) ==
           OwnerRecognitionRequestResult::Rejected);

    assert(tracker.Request("flow-fresh", "event-fresh", 1000, true, 600) ==
           OwnerRecognitionRequestResult::Confirmed);
    assert(tracker.Request("flow-stale", "event-stale", 1000, true, 499) ==
           OwnerRecognitionRequestResult::Pending);
    assert(tracker.NeedsFaceSample(1000));
    assert(tracker.Request("flow-stale", "event-other", 1001, false, 0) ==
           OwnerRecognitionRequestResult::Duplicate);

    assert(tracker.Request("flow-2", "event-2", 1100, false, 0) ==
           OwnerRecognitionRequestResult::Pending);
    tracker.RecordFaceMatch(1200);
    assert(!tracker.NeedsFaceSample(1200));
    assert(tracker.CompleteIfOwnerPresent(1700, false).empty());
    auto completed = tracker.CompleteIfOwnerPresent(1800, true);
    assert(completed.size() == 2);
    assert(completed[0].flow_id == "flow-stale");
    assert(completed[0].causation_id == "event-stale");
    assert(completed[1].flow_id == "flow-2");
    assert(!tracker.HasPending());
    assert(tracker.Request("flow-2", "event-new", 1900, true, 1800) ==
           OwnerRecognitionRequestResult::Duplicate);

    assert(tracker.Request("flow-timeout", "event-timeout", 2000, false, 0) ==
           OwnerRecognitionRequestResult::Pending);
    tracker.Expire(3500);
    assert(tracker.HasPending());
    tracker.Expire(3501);
    assert(!tracker.HasPending());

    for (size_t i = 0; i < OwnerRecognitionFlowTracker::kMaxPendingFlows; ++i) {
        assert(tracker.Request("bulk-flow-" + std::to_string(i),
                               "bulk-event-" + std::to_string(i), 4000, false, 0) ==
               OwnerRecognitionRequestResult::Pending);
    }
    assert(tracker.Request("overflow", "event-overflow", 4000, false, 0) ==
           OwnerRecognitionRequestResult::Rejected);
    tracker.Clear();
    assert(!tracker.HasPending());

    std::cout << "owner_recognition_flow_test: PASS\n";
    return 0;
}
