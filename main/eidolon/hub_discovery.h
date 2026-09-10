#ifndef EIDOLON_HUB_DISCOVERY_H_
#define EIDOLON_HUB_DISCOVERY_H_

#include <esp_err.h>

#include <functional>
#include <string>

#include "hub_types.h"

namespace eidolon {

class HubDiscovery {
public:
    // The saved route is already bound to an Owner. Discovery is only a
    // relocation fallback; every returned document still needs verification
    // by accept before it can become the active directory.
    esp_err_t RefreshOwnerDirectory(
        const AuthorityCandidateRecord& commissioned,
        const std::function<esp_err_t(const AuthorityCandidateRecord&)>& accept);
    esp_err_t Discover(AuthorityCandidateRecord& out,
                       const std::string& owner_domain_id);

private:
    esp_err_t EnsureMdnsInit();
    esp_err_t QueryOnce(AuthorityCandidateRecord& best,
                        const std::string& owner_domain_id, bool* found);
    static std::string NormalizeServiceType(const char* configured);
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_DISCOVERY_H_
