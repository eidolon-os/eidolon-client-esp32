#include <cassert>

#include "eidolon/commissioning_transport_core.h"

namespace {

using namespace eidolon;

void EndpointBudgetRejectsTheVendorDefaultAndAcceptsTheOwnedServer()
{
    static_assert(CommissioningTransportEndpointBudget::kVendorEndpoints == 5);
    static_assert(CommissioningTransportEndpointBudget::kEidolonEndpoints == 4);
    static_assert(CommissioningTransportEndpointBudget::kRequiredUriHandlers == 9);
    assert(!CommissioningTransportEndpointBudget::Supports(8));
    assert(CommissioningTransportEndpointBudget::Supports(9));
}

void PartialStartFailureTransfersEveryOwnedResourceExactlyOnce()
{
    CommissioningTransportResourceCore resources;
    assert(resources.Begin(17));
    assert(resources.Own(17, CommissioningTransportResource::OwnerTrustWorker));
    assert(resources.Own(17, CommissioningTransportResource::NetworkInterfaces));
    assert(resources.Own(17, CommissioningTransportResource::EventHandlers));
    assert(resources.Own(17, CommissioningTransportResource::HttpServer));
    assert(resources.Own(17, CommissioningTransportResource::ProvisioningManager));

    const auto cleanup = resources.ClaimCleanup(17);
    assert(cleanup.owner_trust_worker);
    assert(cleanup.network_interfaces);
    assert(cleanup.event_handlers);
    assert(cleanup.http_server);
    assert(cleanup.provisioning_manager);
    assert(!cleanup.window_timer);

    assert(!resources.ClaimCleanup(17).any());
    assert(!resources.ClaimCleanup(16).any());
    assert(!resources.Own(17, CommissioningTransportResource::WindowTimer));
    assert(resources.CompleteCleanup(17));
    assert(!resources.active());
    assert(!resources.CompleteCleanup(17));
}

void ANewGenerationCannotStartUntilThePreviousCleanupCompletes()
{
    CommissioningTransportResourceCore resources;
    assert(resources.Begin(1));
    assert(!resources.Begin(2));
    assert(resources.ClaimCleanup(1).any() == false);
    assert(resources.CompleteCleanup(1));
    assert(resources.Begin(2));
}

}  // namespace

int main()
{
    EndpointBudgetRejectsTheVendorDefaultAndAcceptsTheOwnedServer();
    PartialStartFailureTransfersEveryOwnedResourceExactlyOnce();
    ANewGenerationCannotStartUntilThePreviousCleanupCompletes();
    return 0;
}
