#include <cassert>

#include "radar_presence_tracker.h"

namespace {

void TestDefaultAbsenceHoldIsFiveSeconds()
{
    RadarPresenceTracker tracker;

    tracker.SetAvailable(true, 0);
    assert(tracker.Update(false, 2000) == RadarPresenceState::Vacant);
    assert(tracker.Update(true, 2100) == RadarPresenceState::Vacant);
    assert(tracker.Update(true, 2300) == RadarPresenceState::Present);
    assert(tracker.Update(false, 7299) == RadarPresenceState::Present);
    assert(tracker.Update(false, 7300) == RadarPresenceState::Vacant);
}

void TestWarmupAndPresenceHysteresis()
{
    RadarPresenceTracker tracker({2000, 200, 60000});

    assert(tracker.state() == RadarPresenceState::Unavailable);
    tracker.SetAvailable(true, 100);
    assert(tracker.state() == RadarPresenceState::Calibrating);
    assert(tracker.Update(false, 2099) == RadarPresenceState::Calibrating);
    assert(tracker.Update(false, 2100) == RadarPresenceState::Vacant);

    assert(tracker.Update(true, 2200) == RadarPresenceState::Vacant);
    assert(tracker.Update(true, 2399) == RadarPresenceState::Vacant);
    assert(tracker.Update(true, 2400) == RadarPresenceState::Present);

    assert(tracker.Update(true, 2500) == RadarPresenceState::Present);
    assert(tracker.Update(false, 62499) == RadarPresenceState::Present);
    assert(tracker.Update(false, 62500) == RadarPresenceState::Vacant);
}

void TestShortPulseAndDockRemoval()
{
    RadarPresenceTracker tracker({100, 200, 1000});

    tracker.SetAvailable(true, 0);
    assert(tracker.Update(false, 100) == RadarPresenceState::Vacant);
    assert(tracker.Update(true, 200) == RadarPresenceState::Vacant);
    assert(tracker.Update(false, 300) == RadarPresenceState::Vacant);
    assert(tracker.Update(true, 400) == RadarPresenceState::Vacant);
    assert(tracker.Update(true, 600) == RadarPresenceState::Present);

    tracker.SetAvailable(false, 700);
    assert(tracker.state() == RadarPresenceState::Unavailable);
    assert(tracker.Update(true, 800) == RadarPresenceState::Unavailable);

    tracker.SetAvailable(true, 900);
    assert(tracker.state() == RadarPresenceState::Calibrating);
}

}  // namespace

int main()
{
    TestDefaultAbsenceHoldIsFiveSeconds();
    TestWarmupAndPresenceHysteresis();
    TestShortPulseAndDockRemoval();
    return 0;
}
