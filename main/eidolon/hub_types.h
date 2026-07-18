#ifndef EIDOLON_HUB_TYPES_H_
#define EIDOLON_HUB_TYPES_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace eidolon {

// mDNS TXT record keys (hub/core/discovery.py)
inline constexpr const char* kTxtVers = "txtvers";
inline constexpr const char* kTxtApi = "api";
inline constexpr const char* kTxtVersion = "version";
inline constexpr const char* kTxtRegisterUrl = "register_url";

inline constexpr int kSupportedTxtVers = 1;
inline constexpr int kSupportedMaxTxtVers = 1;
inline constexpr const char* kExpectedApi = "v1";

inline constexpr const char* kNvsNamespace = "eidolon";

enum class HubConfigStatus {
    PendingApproval,
    WaitingBinding,
    Active,
    Revoked,
    Unregistered,
};

inline const char* HubConfigStatusToString(HubConfigStatus status) {
    switch (status) {
    case HubConfigStatus::PendingApproval:
        return "pending_approval";
    case HubConfigStatus::WaitingBinding:
        return "waiting_binding";
    case HubConfigStatus::Active:
        return "active";
    case HubConfigStatus::Revoked:
        return "revoked";
    case HubConfigStatus::Unregistered:
        return "unregistered";
    }
    return "active";
}

inline HubConfigStatus ParseHubConfigStatus(const std::string& status) {
    if (status == "active") {
        return HubConfigStatus::Active;
    }
    if (status == "waiting_binding") {
        return HubConfigStatus::WaitingBinding;
    }
    if (status == "revoked") {
        return HubConfigStatus::Revoked;
    }
    if (status == "unregistered") {
        return HubConfigStatus::Unregistered;
    }
    // "pending_approval" and any unrecognized value fall through here. Default to
    // the most conservative state: never grant voice on an unknown status.
    return HubConfigStatus::PendingApproval;
}

struct HubTxtRecord {
    int txtvers = 0;
    std::map<std::string, std::string> entries;
    std::string api;
    std::string register_url;
    std::string hub_version;
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
    // `active` holds the pending room while pending/waiting, and the voice room
    // once active. `control` is the per-device control room (only when active).
    RoomConfig active;
    RoomConfig control;
    // Generation id assigned by Hub for the current signed capability manifest.
    // It is also embedded in the control-room participant metadata so stale
    // disconnects cannot retire a newer registration generation.
    std::string registration_id;
    std::string device_fingerprint;
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
    uint32_t sample_interval_ms = 500;
    uint32_t preview_interval_ms = 1000;
    uint32_t motion_threshold = 18;
    uint32_t motion_clear_threshold = 9;
    uint32_t candidate_debounce_ms = 1000;
    uint32_t absence_timeout_ms = 180000;
    uint32_t consecutive_capture_failures = 5;
    uint32_t owner_face_interval_ms = 1500;
    uint32_t owner_presence_enter_ms = 2500;
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
