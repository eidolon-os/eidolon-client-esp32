#ifndef EIDOLON_DEVICE_CONTROL_DELIVERY_CLIENT_H_
#define EIDOLON_DEVICE_CONTROL_DELIVERY_CLIENT_H_

#include <esp_err.h>

#include "device_claim_consumer_core.h"
#include "hub_trust_store.h"

namespace eidolon {

class DeviceControlDeliveryClient {
public:
    // Polls the transport-neutral DeviceDeliveryPort over the currently
    // selected HTTPS adapter. `removal_completed` is true only after signed
    // evidence was accepted by Device Control.
    esp_err_t PollAndExecute(const ActiveClaimState& claim,
                             const OwnerTrustBundle& trust,
                             bool& removal_completed,
                             const std::function<bool()>& current);
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_CONTROL_DELIVERY_CLIENT_H_
