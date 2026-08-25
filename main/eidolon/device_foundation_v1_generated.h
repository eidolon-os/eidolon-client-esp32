// GENERATED from eidolon_sdk/contracts/device_foundation/v1/common/schemas.schema.json.
// Do not edit by hand.
#ifndef EIDOLON_DEVICE_FOUNDATION_V1_GENERATED_H_
#define EIDOLON_DEVICE_FOUNDATION_V1_GENERATED_H_

#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace eidolon::device_foundation::v1 {

struct OwnerDomainId { std::string value; };
struct BusinessOwnerId { std::string value; };

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
    uint64_t owner_domain_generation = 0;
    uint64_t directory_revision = 0;
    // The absolute https URL this document is published at. A consumer GETs it
    // verbatim; deriving it from an endpoint base address is what made
    // commissioning fail at its last step against every Host.
    std::string descriptor_uri;
    std::vector<std::string> trust_root_refs;
    std::vector<AuthorityEndpoint> endpoints;
    std::string issued_at;
    std::string expires_at;
    std::string signing_key_id;
    std::string signature;
};

// How much longer a setup offer lasts, when it lasts a bounded time at all.
//
// The class exists so that "this offer does not end" cannot be written as a
// number. There is no public way to build one, and the only factory refuses
// anything that is not a duration a controller could act on — so the wire can
// never carry 0, which is what the descriptor used to send for an endless
// window and what made every uncommissioned device look like it was advertising
// a window that had already closed. The absence of a deadline is carried by an
// empty std::optional, and therefore by an absent field.
class SetupWindowRemainingSeconds {
  public:
    static std::optional<SetupWindowRemainingSeconds> FromPositiveSeconds(int64_t seconds) {
        if (seconds < 1 || seconds > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }
        return SetupWindowRemainingSeconds(static_cast<uint32_t>(seconds));
    }

    uint32_t seconds() const { return seconds_; }

  private:
    explicit constexpr SetupWindowRemainingSeconds(uint32_t seconds) : seconds_(seconds) {}

    uint32_t seconds_;
};

// Whether this descriptor's identity is bound to a product credential.
// Development and production devices differ in this value only — the setup act
// that follows is the same one either way.
enum class SetupDescriptorTrust {
    DevelopmentTofu,
    ManufacturerBound,
};

inline constexpr const char* SetupDescriptorTrustWireValue(SetupDescriptorTrust trust) {
    return trust == SetupDescriptorTrust::ManufacturerBound ? "manufacturer-bound"
                                                           : "development-tofu";
}

// The descriptor's own wire version. It is not the Device Foundation "1.0":
// this document predates a device having any Owner, and a controller that reads
// a version it does not know must refuse rather than guess.
inline constexpr const char* kSetupDescriptorContractVersion = "1";

// The field names, in the canonical (sorted) order a descriptor is serialised
// in. A producer that writes them in this order can be compared byte-for-byte
// against the golden vector, which is the only thing that makes "a field was
// added to the contract and not to the firmware" a red test rather than a
// device the controller refuses to talk to.
struct SetupDescriptorKeys {
    static constexpr const char* kContractVersion = "contract_version";
    static constexpr const char* kDeviceId = "device_id";
    static constexpr const char* kDeviceKind = "device_kind";
    static constexpr const char* kDisplayName = "display_name";
    static constexpr const char* kExpiresInSeconds = "expires_in_seconds";
    static constexpr const char* kIdentityFingerprint = "identity_fingerprint";
    static constexpr const char* kSessionId = "session_id";
    static constexpr const char* kTrust = "trust";
};

// What a device tells a controller about itself before it belongs to anyone.
struct SetupDescriptor {
    std::string device_id;
    std::string device_kind;
    std::string display_name;
    std::string identity_fingerprint;
    std::string session_id;
    // A duration, not an instant: a device being set up has not joined a
    // network and has no wall clock, so it can say how long this window lasts
    // but not when it ends. Empty when the offer does not end at all — a device
    // nobody has claimed keeps advertising until it is claimed, cancelled or
    // powered off, and so has no duration to name.
    std::optional<SetupWindowRemainingSeconds> expires_in;
    SetupDescriptorTrust trust = SetupDescriptorTrust::DevelopmentTofu;
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

struct DeviceRef {
    std::string device_instance_id;
    OwnerDomainId owner_domain_id;
    uint64_t owner_domain_generation = 0;
    uint32_t claim_generation = 0;
    uint32_t trust_epoch = 0;
};

struct CommandEnvelope { std::string contract = "eidolon.device-foundation.command"; std::string contract_version = "1.0"; std::string command_type; std::string command_id; std::string correlation_id; std::optional<std::string> causation_id; std::string issued_at; std::optional<std::string> deadline; std::string payload_json; std::string extensions_json; };
struct CommandResult { std::string command_id; std::string outcome; std::string resource_ref_json; std::optional<uint64_t> resource_revision; std::string occurred_at; std::string extensions_json; };
struct DeviceProblem { std::string code; std::string category; bool retryable = false; std::string authority; std::optional<std::string> command_id; std::string resource_ref_json; std::optional<uint64_t> current_revision; std::optional<uint64_t> current_generation; std::optional<uint64_t> retry_after_ms; std::string detail; std::string incident_id; };
struct RevokeClaim { std::string operation = "device.claim-revocation"; std::string command_id; std::string correlation_id; DeviceRef device_ref; std::string reason; };
struct RevokeClaimResult { std::string operation = "device.claim-revocation-result"; std::string command_id; std::string outcome; DeviceRef device_ref; uint64_t aggregate_revision = 0; std::string occurred_at; std::optional<std::string> event_id; std::string lifecycle_state = "revoked"; };

struct ManifestRef {
    std::string manifest_id;
    uint64_t revision = 0;
    std::string digest;
};

enum class EnrollmentProposalState {
    PendingReview, ApprovedAwaitingHandoff, GrantDelivered,
    GrantAcknowledged, Rejected, Expired, Canceled, ClaimRevoked,
};

enum class ClaimState { Active, Suspended, Revoked };

inline constexpr const char* kAdmissionEventSource = "urn:eidolon:authority:admission";
enum class AdmissionEventType {
    ProposalCreated, Approved, Rejected, Expired, Canceled, GrantDelivered,
    GrantAcknowledged, ClaimActivated, ClaimSuspended, ClaimResumed,
    ClaimRevoked, TrustEpochChanged, ManifestAccepted,
};

struct ControllerActorRef {
    std::string principal_id;
    OwnerDomainId owner_domain_id;
    std::vector<std::string> granted_scopes;
    std::string authentication_strength;
};

struct EnrollmentProposal {
    std::string enrollment_id;
    uint64_t proposal_revision = 0;
    EnrollmentProposalState state = EnrollmentProposalState::PendingReview;
    std::string device_instance_candidate_id;
    OwnerDomainId requested_owner_domain_id;
    std::string hardware_evidence_digest;
    ManifestRef manifest_ref;
    std::string handoff_key_id;
    std::string created_at;
    std::string expires_at;
};

struct ApprovalDecision {
    std::string decision_id;
    std::string enrollment_id;
    std::string decision;
    ControllerActorRef actor;
    OwnerDomainId target_owner_domain_id;
    BusinessOwnerId target_business_owner_id;
    ManifestRef reviewed_manifest_ref;
    uint64_t expected_proposal_revision = 0;
    std::string decided_at;
};

struct ClaimGrant {
    std::string grant_id;
    std::string enrollment_id;
    DeviceRef device_ref;
    ManifestRef manifest_ref;
    std::string approval_decision_id;
    std::string handoff_key_id;
    std::string operational_key_id;
    std::string issued_at;
    std::string expires_at;
};

struct GrantAck { std::string enrollment_id; std::string grant_id; DeviceRef device_ref; std::string acknowledged_at; };
struct ClaimRecord { DeviceRef device_ref; BusinessOwnerId business_owner_id; ManifestRef manifest_ref; ClaimState state = ClaimState::Active; uint64_t revision = 0; std::string updated_at; };

struct CreateEnrollment { std::string profile_id; std::string device_instance_candidate_id; OwnerDomainId requested_owner_domain_id; std::string hardware_identity_evidence; std::string commissioning_proof; std::string manifest; std::string handoff_key; std::string operational_key; };
struct CreateEnrollmentResult { std::string enrollment_id; uint64_t proposal_revision = 0; std::string state; std::string expires_at; std::string reviewed_manifest_digest; std::string collection_challenge; };
struct CancelEnrollment { std::string enrollment_id; std::string reason; };
struct CancelEnrollmentResult { std::string enrollment_id; std::string proposal_state; std::string canceled_at; };
struct DecideEnrollment { std::string enrollment_id; uint64_t expected_proposal_revision = 0; std::string decision; OwnerDomainId target_owner_domain_id; BusinessOwnerId target_business_owner_id; std::optional<std::string> target_space_id; ManifestRef reviewed_manifest_ref; std::vector<std::string> initial_capability_policy_refs; };
struct DecideEnrollmentResult { std::string decision_id; std::string decision; ControllerActorRef decided_by; std::string decided_at; uint64_t proposal_revision = 0; };
struct CollectClaimGrant { std::string enrollment_id; uint64_t proposal_revision = 0; std::string collection_challenge; std::string handoff_key_proof; };

struct ClaimGrantAAD {
    std::string contract = "eidolon.device-foundation.claim-grant-aad";
    std::string profile_id = "eidolon-trust-p256-hpke-v1";
    std::string enrollment_id;
    uint64_t proposal_revision = 0;
    std::string device_instance_id;
    std::string hardware_evidence_digest;
    ManifestRef manifest_ref;
    OwnerDomainId owner_domain_id;
    uint64_t owner_domain_generation = 0;
    uint32_t claim_generation = 0;
    uint32_t trust_epoch = 0;
    std::string grant_id;
};

struct ClaimGrantWireEnvelope {
    std::string contract = "eidolon.device-foundation.claim-grant-envelope";
    std::string profile_id = "eidolon-trust-p256-hpke-v1";
    std::string kem = "DHKEM-P256-HKDF-SHA256";
    std::string kdf = "HKDF-SHA256";
    std::string aead = "AES-128-GCM";
    std::string recipient_handoff_key_id;
    std::string encapsulated_key;
    std::string ciphertext;
    ClaimGrantAAD aad;
};

struct CollectClaimGrantResult { std::string grant_id; ClaimGrantWireEnvelope wire_envelope; std::string expires_at; std::string approval_decision_id; };
struct AckClaimGrant { std::string enrollment_id; std::string grant_id; std::string operational_key_proof; uint32_t stored_claim_generation = 0; uint32_t stored_trust_epoch = 0; };
struct AckClaimGrantResult { DeviceRef device_ref; ClaimState claim_state = ClaimState::Active; };

struct AdmissionListCursor { OwnerDomainId owner_domain_id; std::string sort_key; std::string resource_id; };
struct GrantDeliveryRecord { std::string grant_id; std::string approval_decision_id; std::string state; std::string delivered_at; std::optional<std::string> acknowledged_at; };
struct EnrollmentRecoveryProjection { EnrollmentProposal proposal; std::optional<ApprovalDecision> approval_decision; std::optional<GrantDeliveryRecord> grant_delivery; std::optional<ClaimRecord> claim; uint64_t source_revision = 0; std::string observed_at; };
struct EnrollmentProposalQuery { OwnerDomainId owner_domain_id; std::vector<EnrollmentProposalState> states; std::optional<AdmissionListCursor> cursor; uint16_t limit = 0; };
struct EnrollmentProposalPage { OwnerDomainId owner_domain_id; std::vector<EnrollmentRecoveryProjection> items; std::optional<AdmissionListCursor> next_cursor; std::string observed_at; };
struct ClaimQuery { OwnerDomainId owner_domain_id; std::vector<ClaimState> states; std::optional<AdmissionListCursor> cursor; uint16_t limit = 0; };
struct ClaimPage { OwnerDomainId owner_domain_id; std::vector<ClaimRecord> items; std::optional<AdmissionListCursor> next_cursor; std::string observed_at; };

struct ClaimActivatedData { DeviceRef device_ref; BusinessOwnerId business_owner_id; ManifestRef manifest_ref; std::string approval_decision_id; std::string activated_at; };
struct ClaimRevokedData { DeviceRef device_ref; std::string reason; std::string revoked_at; };
template <typename Data> struct ClaimCloudEvent { std::string specversion = "1.0"; std::string id; std::string source = "urn:eidolon:authority:admission"; std::string type; std::string subject; std::string time; std::string datacontenttype = "application/json"; std::string dataschema; std::string audience = "eidolon-claim-consumers"; OwnerDomainId ownerdomainid; uint64_t aggregaterev = 0; std::string correlationid; std::string causationid; Data data; };
using ClaimActivatedEvent = ClaimCloudEvent<ClaimActivatedData>;
using ClaimRevokedEvent = ClaimCloudEvent<ClaimRevokedData>;
struct ClaimEventCursor { std::string stream_id = "admission-claims-v1"; uint64_t stream_position = 0; };
using ClaimLifecycleEvent = std::variant<ClaimActivatedEvent, ClaimRevokedEvent>;
struct ClaimEventStreamItem { uint64_t stream_position = 0; ClaimLifecycleEvent event; };
struct ClaimEventPage { std::string stream_id = "admission-claims-v1"; ClaimEventCursor requested_after; std::vector<ClaimEventStreamItem> events; ClaimEventCursor next_cursor; uint64_t high_watermark = 0; std::string observed_at; };

inline bool HasExactFields(const std::set<std::string>& actual,
                           const std::set<std::string>& expected) {
    return actual == expected;
}

inline bool IsValid(const ClaimGrantAAD& value) {
    return value.contract == "eidolon.device-foundation.claim-grant-aad" &&
           value.profile_id == "eidolon-trust-p256-hpke-v1" &&
           value.owner_domain_id.value.rfind("owner-", 0) == 0 &&
           value.proposal_revision > 0 && value.owner_domain_generation > 0 &&
           value.claim_generation > 0 && value.trust_epoch > 0;
}

inline bool IsValid(const ClaimGrantWireEnvelope& value) {
    return value.contract == "eidolon.device-foundation.claim-grant-envelope" &&
           value.profile_id == "eidolon-trust-p256-hpke-v1" &&
           value.kem == "DHKEM-P256-HKDF-SHA256" &&
           value.kdf == "HKDF-SHA256" && value.aead == "AES-128-GCM" &&
           IsValid(value.aad);
}

enum class DeviceLocalEraseResult {
    Erased,
    PermanentFailure,
};

struct DeviceLocalEraseCommand {
    std::string operation_id;
    DeviceRef device_ref;
    std::string deadline;
    std::vector<std::string> erase_scopes;
};

struct DeviceLocalEraseAck {
    std::string operation_id;
    DeviceRef device_ref;
    uint32_t ack_sequence = 0;
    DeviceLocalEraseResult result = DeviceLocalEraseResult::PermanentFailure;
    std::string result_code;
    uint64_t device_monotonic_time = 0;
    std::string device_signature;
};

enum class DeliveryKind {
    TwinDelta,
    Operation,
    StopGeneration,
};

struct DeliverEnvelope {
    std::string delivery_attempt_id;
    std::string message_id;
    DeliveryKind kind = DeliveryKind::Operation;
    DeviceRef device_ref;
    std::string deadline;
    std::string payload_schema;
    std::string payload_json;
};

enum class DeliveryAcceptanceState {
    Accepted,
    Rejected,
};

struct DeliveryAcceptance {
    std::string delivery_attempt_id;
    DeliveryAcceptanceState state = DeliveryAcceptanceState::Rejected;
    std::string adapter_code;
};

struct DeviceEvidenceEnvelope {
    std::string delivery_attempt_id;
    std::string message_id;
    DeviceRef device_ref;
    std::string payload_schema;
    std::string payload_json;
};

}  // namespace eidolon::device_foundation::v1

#endif  // EIDOLON_DEVICE_FOUNDATION_V1_GENERATED_H_
