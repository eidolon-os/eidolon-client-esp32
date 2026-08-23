#ifndef EIDOLON_COMMISSIONING_TRANSPORT_CORE_H_
#define EIDOLON_COMMISSIONING_TRANSPORT_CORE_H_

#include <cstdint>

namespace eidolon {

// ESP-IDF's SoftAP provisioning transport installs five built-in HTTP
// endpoints. Eidolon adds four canonical endpoints. The adapter must own this
// capacity explicitly instead of inheriting HTTPD_DEFAULT_CONFIG (8), which is
// one slot too small and makes every setup attempt fail deterministically.
struct CommissioningTransportEndpointBudget {
    static constexpr uint16_t kVendorEndpoints = 5;
    static constexpr uint16_t kEidolonEndpoints = 4;
    static constexpr uint16_t kRequiredUriHandlers =
        kVendorEndpoints + kEidolonEndpoints;

    static constexpr bool Supports(uint16_t available)
    {
        return available >= kRequiredUriHandlers;
    }
};

enum class CommissioningTransportResource {
    OwnerTrustWorker,
    NetworkInterfaces,
    EventHandlers,
    HttpServer,
    ProvisioningManager,
    // The provisioning SDK starts the shared ESP-IDF Wi-Fi driver. The
    // commissioning transport therefore owns returning that driver to the
    // stopped boundary before its netifs are destroyed and Station takes the
    // RadioLease back.
    WifiDriver,
    WindowTimer,
};

struct CommissioningTransportCleanupPlan {
    bool owner_trust_worker = false;
    bool network_interfaces = false;
    bool event_handlers = false;
    bool http_server = false;
    bool provisioning_manager = false;
    bool wifi_driver = false;
    bool window_timer = false;

    bool any() const
    {
        return owner_trust_worker || network_interfaces || event_handlers ||
               http_server || provisioning_manager || wifi_driver ||
               window_timer;
    }

    bool CanReleaseNetworkInterfaces(bool wifi_driver_stopped) const
    {
        return !network_interfaces || !wifi_driver || wifi_driver_stopped;
    }
};

// Pure, Host/chip/SDK-independent ownership ledger for one transport
// generation. Only the commissioning actor calls this type. ClaimCleanup()
// transfers teardown ownership exactly once, so normal stop, partial start
// failure and late SDK events cannot free the same resource set twice.
class CommissioningTransportResourceCore {
public:
    bool Begin(uint32_t generation);
    bool Own(uint32_t generation, CommissioningTransportResource resource);
    CommissioningTransportCleanupPlan ClaimCleanup(uint32_t generation);
    bool CompleteCleanup(uint32_t generation);

    bool active() const { return generation_ != 0; }
    uint32_t generation() const { return generation_; }
    bool cleanup_claimed() const { return cleanup_claimed_; }

private:
    uint32_t generation_ = 0;
    CommissioningTransportCleanupPlan owned_;
    bool cleanup_claimed_ = false;
};

}  // namespace eidolon

#endif  // EIDOLON_COMMISSIONING_TRANSPORT_CORE_H_
