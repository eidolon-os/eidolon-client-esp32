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

}  // namespace

int main()
{
    TestOperationalRequiresEveryConfirmedBoundary();
    TestCommissioningFencesOtherwiseReadyRuntime();
    return 0;
}
