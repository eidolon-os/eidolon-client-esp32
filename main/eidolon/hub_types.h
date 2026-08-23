#ifndef EIDOLON_HUB_TYPES_H_
#define EIDOLON_HUB_TYPES_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "device_foundation_v1_generated.h"

namespace eidolon {

// mDNS TXT record keys (eidolon_hub discovery contract).
inline constexpr const char* kTxtVers = "txtvers";
inline constexpr const char* kTxtOwnerDomainId = "owner_domain_id";
inline constexpr const char* kTxtOwnerDomainDescriptorUri =
    "owner_domain_descriptor_uri";

inline constexpr int kSupportedTxtVers = 1;
inline constexpr int kSupportedOnboardingProtocol = 1;
inline constexpr const char* kLiveKitBindingFormat =
    "application/vnd.eidolon.livekit-session+json;v=2";

inline constexpr const char* kNvsNamespace = "eidolon";

enum class HubConfigStatus {
    PendingApproval,
    WaitingBinding,
    Active,
    Revoked,
};

inline const char* HubConfigStatusToString(HubConfigStatus status) {
    switch (status) {
    case HubConfigStatus::PendingApproval:
        return "pending-approval";
    case HubConfigStatus::WaitingBinding:
        return "waiting-binding";
    case HubConfigStatus::Active:
        return "approved";
    case HubConfigStatus::Revoked:
        return "revoked";
    }
    return "active";
}

inline HubConfigStatus ParseHubConfigStatus(const std::string& status) {
    if (status == "approved") {
        return HubConfigStatus::Active;
    }
    if (status == "waiting-binding") {
        return HubConfigStatus::WaitingBinding;
    }
    if (status == "revoked") {
        return HubConfigStatus::Revoked;
    }
    // "pending-approval" and any unrecognized value fall through here. Default to
    // the most conservative state: never grant voice on an unknown status.
    return HubConfigStatus::PendingApproval;
}

struct AuthorityCandidateRecord {
    int txtvers = 0;
    std::map<std::string, std::string> entries;
    std::string owner_domain_id;
    std::string owner_domain_descriptor_uri;
};

// Crash-safe, short-lived enrollment secrets. This state is deliberately
// independent from Wi-Fi credentials and from the approved channel config.
struct HubOnboardingState {
    std::string owner_domain_id;
    uint64_t owner_domain_generation = 0;
    uint64_t directory_revision = 0;
    std::string device_id;
    std::string request_id;
    std::string retrieval_token;
    std::string enrollment_id;
    std::string lifecycle_state = "pending-approval";

    bool has_local_intent() const {
        return !request_id.empty() && !retrieval_token.empty();
    }
    bool enrolled() const { return !enrollment_id.empty(); }
};

// Long-lived Claim identity. Enrollment intent and retrieval capability never
// cross this boundary: once ClaimActive is observed, only the exact DeviceRef
// and its device-held operation key authorize Device Control.
struct ActiveClaimState {
    device_foundation::v1::DeviceRef device_ref;
    std::string lifecycle_state = "approved";

    bool valid() const {
        return !device_ref.device_instance_id.empty() &&
               !device_ref.owner_domain_id.empty() &&
               device_ref.owner_domain_generation > 0 &&
               device_ref.claim_generation > 0 &&
               device_ref.trust_epoch > 0 &&
               !device_ref.accepted_manifest_digest.empty();
    }
};

struct HubEnrollmentReceipt {
    std::string request_id;
    std::string enrollment_id;
    std::string device_id;
    std::string lifecycle_state;
    int64_t retrieval_expires_at_ms = 0;
};

struct HubChannelAssignment {
    std::string channel_id;
    std::string binding_format;
    std::string opaque_binding;
    int64_t expires_at_ms = 0;
};

// A single LiveKit room the device can connect to.
struct RoomConfig {
    std::string server_url;
    std::string token;
    std::string identity;
    std::string room_name;

    bool usable() const { return !server_url.empty() && !token.empty(); }
};

struct Esp32HubConfig {
    // Default to the most conservative status: a config that has not been
    // explicitly populated/parsed must never grant voice access.
    HubConfigStatus status = HubConfigStatus::PendingApproval;
    // The one channel this device has. It used to be two — a control room it
    // lived in and a voice room it visited — which cost a room teardown and
    // rebuild at the start of every conversation, and left the device
    // unreachable in between. One channel is held open for as long as the
    // device is enrolled; whether anyone is listening is now said out loud
    // rather than inferred from which room it is standing in.
    RoomConfig session;
    int64_t expires_at_ms = 0;
    int sample_rate = 16000;
    int channels = 1;
};

// Guard-local runtime configuration returned by GET /api/guard/runtime-config.
// It is deliberately separate from Esp32HubConfig: a guard does not require a
// persona, voice room, memory realm, or ordinary active configuration.
struct GuardRuntimeHubConfig {
    std::string binding_id;
    std::string guard_companion_id;
    std::string desired_runtime_state;
    uint32_t runtime_revision = 0;
    uint32_t sample_interval_ms = 200;
    uint32_t preview_interval_ms = 1000;
    uint32_t motion_threshold = 18;
    uint32_t motion_clear_threshold = 9;
    uint32_t candidate_debounce_ms = 1000;
    uint32_t absence_timeout_ms = 180000;
    uint32_t consecutive_capture_failures = 5;
    uint32_t owner_face_interval_ms = 500;
    uint32_t owner_presence_enter_ms = 600;
    uint32_t owner_presence_exit_ms = 12000;
    uint32_t owner_presence_heartbeat_ms = 10000;
    uint32_t owner_presence_lease_ms = 30000;
    RoomConfig control;
};

struct OwnerFaceReferenceHubConfig {
    std::string reference_id;
    std::string pose;
    std::string sha256;
    uint32_t size_bytes = 0;
    std::string content_type;
};

struct OwnerFaceProfileHubConfig {
    std::string binding_id;
    std::string profile_id;
    uint32_t profile_revision = 0;
    std::string desired_state;
    std::string model_id;
    std::string preprocessing_version;
    std::vector<OwnerFaceReferenceHubConfig> references;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_TYPES_H_
