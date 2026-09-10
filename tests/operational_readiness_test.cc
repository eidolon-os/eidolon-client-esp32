#include <cassert>

#include "eidolon/operational_readiness.h"

namespace {

void TestOperationalRequiresEveryConfirmedBoundary()
{
    eidolon::OperationalReadinessSnapshot snapshot{
        .owner_network_ready = true,
        .hub_activation_ready = true,
        .transport_ready = true,
        .commissioning_in_progress = false,
    };
    assert(eidolon::IsOperationalConfirmed(snapshot));

    snapshot.transport_ready = false;
    assert(!eidolon::IsOperationalConfirmed(snapshot));
    snapshot.transport_ready = true;
    snapshot.hub_activation_ready = false;
    assert(!eidolon::IsOperationalConfirmed(snapshot));
    snapshot.hub_activation_ready = true;
    snapshot.owner_network_ready = false;
    assert(!eidolon::IsOperationalConfirmed(snapshot));
}

void TestCommissioningFencesOtherwiseReadyRuntime()
{
    const eidolon::OperationalReadinessSnapshot snapshot{
        .owner_network_ready = true,
        .hub_activation_ready = true,
        .transport_ready = true,
        .commissioning_in_progress = true,
    };
    assert(!eidolon::IsOperationalConfirmed(snapshot));
}

void TestRadioHandoffAndConnectionOrdering()
{
    using eidolon::ShouldResumeOperationalNetwork;
    // Connected before lease release: no admission until the Idle observer
    // reconciles fresh network evidence. Then the delayed callback is a no-op.
    assert(!ShouldResumeOperationalNetwork(true, false, 2, 1, true));
    assert(ShouldResumeOperationalNetwork(true, false, 2, 1, false));
    assert(!ShouldResumeOperationalNetwork(true, true, 2, 2, false));

    // Lease release before connection: setup can finish offline, and the
    // ordinary network callback resumes it when the saved AP returns.
    assert(!ShouldResumeOperationalNetwork(false, false, 2, 1, false));
    assert(ShouldResumeOperationalNetwork(true, false, 2, 1, false));

    // Even if every intermediate disconnect was hidden by commissioning, a
    // new generation must resume the quiesced runtime exactly once.
    assert(ShouldResumeOperationalNetwork(true, true, 3, 2, false));
    assert(!ShouldResumeOperationalNetwork(true, true, 3, 3, false));
    // Ordinary link loss/recovery in one generation still resumes the channel.
    assert(ShouldResumeOperationalNetwork(true, false, 3, 3, false));
}

}  // namespace

int main()
{
    TestRadioHandoffAndConnectionOrdering();
    TestOperationalRequiresEveryConfirmedBoundary();
    TestCommissioningFencesOtherwiseReadyRuntime();
    return 0;
}
