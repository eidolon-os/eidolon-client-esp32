#ifndef EIDOLON_DEVICE_DELIVERY_CONSUMER_CORE_H_
#define EIDOLON_DEVICE_DELIVERY_CONSUMER_CORE_H_

#include <string>

#include "device_foundation_v1_generated.h"
#include "device_local_erase_core.h"

namespace eidolon {

inline constexpr const char* kDeviceLocalEraseCommandSchema =
    "https://contracts.eidolon.live/device-foundation/v1/device-control/"
    "schemas.schema.json#/$defs/DeviceLocalEraseCommand";
inline constexpr const char* kDeviceLocalEraseAckSchema =
    "https://contracts.eidolon.live/device-foundation/v1/device-control/"
    "schemas.schema.json#/$defs/DeviceLocalEraseAck";

class DeviceDeliveryFingerprintPort {
public:
    virtual ~DeviceDeliveryFingerprintPort() = default;
    virtual bool Sha256(const std::string& canonical,
                        std::string& digest) = 0;
};

enum class DeviceDeliveryConsumerResult {
    EvidenceReady,
    AcceptedWithoutEvidence,
    RejectedInvalidContract,
    RejectedUnsupportedKind,
    RejectedStaleGeneration,
    RejectedExpired,
    RetryableFailure,
};

struct DeviceDeliveryConsumerOutcome {
    DeviceDeliveryConsumerResult result =
        DeviceDeliveryConsumerResult::RejectedInvalidContract;
    device_foundation::v1::DeliveryAcceptance acceptance;
    bool has_evidence = false;
    device_foundation::v1::DeviceEvidenceEnvelope evidence;
    std::string evidence_json;
};

class DeviceDeliveryConsumerCore {
public:
    DeviceDeliveryConsumerCore(DeviceLocalEraseCore& erase,
                               DeviceDeliveryFingerprintPort& fingerprint)
        : erase_(erase), fingerprint_(fingerprint) {}

    DeviceDeliveryConsumerOutcome Handle(const std::string& envelope_json);

    static std::string EvidenceJson(
        const device_foundation::v1::DeviceEvidenceEnvelope& envelope);

private:
    DeviceLocalEraseCore& erase_;
    DeviceDeliveryFingerprintPort& fingerprint_;
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_DELIVERY_CONSUMER_CORE_H_
