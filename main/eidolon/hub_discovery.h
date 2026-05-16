#ifndef EIDOLON_HUB_DISCOVERY_H_
#define EIDOLON_HUB_DISCOVERY_H_

#include <esp_err.h>

#include <string>

#include "hub_types.h"

namespace eidolon {

class HubDiscovery {
public:
    esp_err_t Discover(HubTxtRecord& out, const std::string& preferred_instance_substr = "Eidolon Hub");

private:
    esp_err_t EnsureMdnsInit();
    esp_err_t QueryOnce(HubTxtRecord& best, const std::string& preferred_instance_substr, bool* found);
    static std::string NormalizeServiceType(const char* configured);
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_DISCOVERY_H_
