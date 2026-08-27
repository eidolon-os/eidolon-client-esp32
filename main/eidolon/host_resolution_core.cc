#include "host_resolution_core.h"

namespace eidolon {

std::string HostOfUrl(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    const size_t start = scheme + 3;
    size_t end = url.find_first_of("/:", start);
    if (end == std::string::npos) {
        end = url.size();
    }
    return url.substr(start, end - start);
}

bool IsLinkLocalName(const std::string& host)
{
    static const std::string kSuffix = ".local";
    return host.size() > kSuffix.size() &&
           host.compare(host.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

bool ShouldDropCachedResolution(const std::string& host, HubRequestFailure failure)
{
    return failure == HubRequestFailure::ConnectionNotOpened &&
           IsLinkLocalName(host);
}

}  // namespace eidolon
