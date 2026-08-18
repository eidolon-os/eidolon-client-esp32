#ifndef EIDOLON_HUB_CONFIG_CLIENT_H_
#define EIDOLON_HUB_CONFIG_CLIENT_H_

#include <esp_err.h>

#include <string>

#include "sdkconfig.h"
#include "hub_types.h"

namespace eidolon {

class HubConfigClient {
public:
#if CONFIG_EIDOLON_GUARD_SERVICE
    // Fetch GuardBinding-local runtime config with the same signed device
    // identity as /api/config. It never fetches persona or policy config.
    esp_err_t FetchGuardRuntime(const std::string& authority_base_uri, const std::string& device_id,
                                GuardRuntimeHubConfig& out);
    esp_err_t FetchOwnerFaceProfile(const std::string& authority_base_uri,
                                    const std::string& device_id,
                                    OwnerFaceProfileHubConfig& out);
    esp_err_t FetchOwnerFaceReference(const std::string& authority_base_uri,
                                      const std::string& device_id,
                                      const OwnerFaceReferenceHubConfig& reference,
                                      std::string& out);
#endif

};

}  // namespace eidolon

#endif  // EIDOLON_HUB_CONFIG_CLIENT_H_
