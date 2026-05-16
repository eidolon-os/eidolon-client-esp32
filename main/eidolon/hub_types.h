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

struct HubTxtRecord {
    int txtvers = 0;
    std::map<std::string, std::string> entries;
    std::string api;
    std::string config_url;
    std::string hub_version;
};

struct Esp32HubConfig {
    std::string server_url;
    std::string token;
    std::string identity;
    std::string room_name;
    int sample_rate = 16000;
    int channels = 1;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_TYPES_H_
