#ifndef EIDOLON_HUB_TYPES_H_
#define EIDOLON_HUB_TYPES_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace eidolon {

// mDNS TXT record keys (eidolon_hub discovery contract).
inline constexpr const char* kTxtVers = "txtvers";
inline constexpr const char* kTxtDescriptorUri = "descriptor_uri";
inline constexpr const char* kTxtEnrollmentUri = "enrollment_uri";

inline constexpr int kSupportedTxtVers = 1;
inline constexpr int kSupportedOnboardingProtocol = 1;
inline constexpr const char* kLiveKitBindingFormat =
    "application/vnd.eidolon.livekit-device+json;v=1";

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

struct HubTxtRecord {
    int txtvers = 0;
    std::map<std::string, std::string> entries;
    std::string descriptor_uri;
    std::string enrollment_uri;
};

struct HubDescriptor {
    int schema_version = 0;
    std::string hub_id;
    std::string descriptor_uri;
    std::string device_onboarding_uri;
    std::string enrollment_uri;
};

// Crash-safe, short-lived enrollment secrets. This state is deliberately
// independent from Wi-Fi credentials and from the approved channel config.
struct HubOnboardingState {
    std::string hub_id;
    std::string descriptor_uri;
    std::string enrollment_uri;
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
    // Provider-owned voice and control rooms. Both are usable only while Active.
    RoomConfig active;
    RoomConfig control;
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
