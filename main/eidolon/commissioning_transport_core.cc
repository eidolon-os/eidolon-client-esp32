#include "commissioning_transport_core.h"

namespace eidolon {

bool CommissioningTransportResourceCore::Begin(uint32_t generation)
{
    if (generation == 0 || active()) return false;
    generation_ = generation;
    owned_ = {};
    cleanup_claimed_ = false;
    return true;
}

bool CommissioningTransportResourceCore::Own(
    uint32_t generation, CommissioningTransportResource resource)
{
    if (generation == 0 || generation != generation_ || cleanup_claimed_) {
        return false;
    }
    switch (resource) {
    case CommissioningTransportResource::OwnerTrustWorker:
        owned_.owner_trust_worker = true;
        break;
    case CommissioningTransportResource::NetworkInterfaces:
        owned_.network_interfaces = true;
        break;
    case CommissioningTransportResource::EventHandlers:
        owned_.event_handlers = true;
        break;
    case CommissioningTransportResource::HttpServer:
        owned_.http_server = true;
        break;
    case CommissioningTransportResource::ProvisioningManager:
        owned_.provisioning_manager = true;
        break;
    case CommissioningTransportResource::WindowTimer:
        owned_.window_timer = true;
        break;
    }
    return true;
}

CommissioningTransportCleanupPlan
CommissioningTransportResourceCore::ClaimCleanup(uint32_t generation)
{
    if (generation == 0 || generation != generation_ || cleanup_claimed_) {
        return {};
    }
    cleanup_claimed_ = true;
    return owned_;
}

bool CommissioningTransportResourceCore::CompleteCleanup(uint32_t generation)
{
    if (generation == 0 || generation != generation_ || !cleanup_claimed_) {
        return false;
    }
    generation_ = 0;
    owned_ = {};
    cleanup_claimed_ = false;
    return true;
}

}  // namespace eidolon
