#ifndef EIDOLON_HUB_TYPES_H_
#define EIDOLON_HUB_TYPES_H_

#include <map>
#include <string>

namespace eidolon {

// mDNS TXT record keys (hub/core/discovery.py)
inline constexpr const char* kTxtVers = "txtvers";
inline constexpr const char* kTxtApi = "api";
inline constexpr const char* kTxtVersion = "version";
inline constexpr const char* kTxtConfigUrl = "config_url";

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
    std::string config_url;
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
    HubConfigStatus status = HubConfigStatus::Active;
    // `active` holds the pending room while pending/waiting, and the voice room
    // once active. `control` is the per-device control room (only when active).
    RoomConfig active;
    RoomConfig control;
    std::string device_fingerprint;
    int sample_rate = 16000;
    int channels = 1;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_TYPES_H_
