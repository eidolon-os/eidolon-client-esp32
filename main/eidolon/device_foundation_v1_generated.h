// GENERATED from eidolon_sdk/contracts/device_foundation/v1/common/schemas.schema.json.
// Do not edit by hand.
#ifndef EIDOLON_DEVICE_FOUNDATION_V1_GENERATED_H_
#define EIDOLON_DEVICE_FOUNDATION_V1_GENERATED_H_

#include <cstdint>
#include <string>
#include <vector>

namespace eidolon::device_foundation::v1 {

enum class LogicalAuthority {
    Admission,
    DeviceControl,
    BodyMesh,
    Companion,
};

struct AuthorityEndpoint {
    LogicalAuthority authority = LogicalAuthority::Admission;
    std::string logical_audience;
    std::string uri;
    std::string transport_profile;
    uint16_t priority = 0;
};

struct OwnerDomainDescriptor {
    std::string owner_domain_id;
    uint64_t directory_revision = 0;
    std::vector<std::string> trust_root_refs;
    std::vector<AuthorityEndpoint> endpoints;
    std::string issued_at;
    std::string expires_at;
    std::string signing_key_id;
    std::string signature;
};

enum class CommissioningStatusState {
    ApplyingConfiguration,
    Committed,
    RolledBack,
    Failed,
};

enum class CommissioningFailureCode {
    None,
    NetworkRejected,
    OwnerRouteUnavailable,
    OwnerIdentityMismatch,
    StorageUnavailable,
    WindowExpired,
    Cancelled,
    Internal,
};

struct CommissioningConditions {
    bool wifi_connected = false;
    bool owner_route_validated = false;
    bool trust_committed = false;
    bool network_committed = false;
};

struct CommissioningStatusEvidence {
    std::string session_id;
    uint32_t setup_generation = 0;
    uint32_t state_revision = 0;
    CommissioningStatusState state =
        CommissioningStatusState::ApplyingConfiguration;
    CommissioningConditions conditions;
    CommissioningFailureCode failure_code = CommissioningFailureCode::None;
};

struct CommissioningTerminalAck {
    std::string session_id;
    uint32_t setup_generation = 0;
    uint32_t observed_state_revision = 0;
};

}  // namespace eidolon::device_foundation::v1

#endif  // EIDOLON_DEVICE_FOUNDATION_V1_GENERATED_H_
