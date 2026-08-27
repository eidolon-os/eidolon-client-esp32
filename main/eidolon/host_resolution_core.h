#ifndef EIDOLON_HOST_RESOLUTION_CORE_H_
#define EIDOLON_HOST_RESOLUTION_CORE_H_

#include <string>

namespace eidolon {

// What a failed Hub request says about the address it was aimed at.
enum class HubRequestFailure {
    // The connection never opened. A wrong address looks exactly like this,
    // and so do a Host that is switched off and a network still coming back.
    ConnectionNotOpened,
    // The Authority answered. Whatever went wrong after that, the address was
    // right, and nothing about the name is in question.
    AuthorityAnswered,
};

// The host part of an absolute URL, or empty when the URL has none. One parser
// serves both the failure diagnostic and the decision below, so the address
// this device reports is the address the decision is made about.
std::string HostOfUrl(const std::string& url);

// Names this device resolves for itself over mDNS, rather than asking a
// resolver on the network.
bool IsLinkLocalName(const std::string& host);

// Whether a failed request should drop the cached resolution of its host.
//
// An mDNS answer is cached for the answer's TTL, and every retry re-resolves
// through that same entry. So one wrong address outlives the moment that
// produced it, and a device that keeps asking asks down the same dead path
// until the entry ages out — a transient fault made permanent, which is the
// one thing the activation loop exists to prevent. Dropping the entry is what
// makes the next attempt a genuinely new attempt.
//
// Only for a link-local name, and only when nothing answered. A name resolved
// by a real resolver on the network is not this device's to second-guess, and
// an Authority that answered has already proved the address was right.
bool ShouldDropCachedResolution(const std::string& host, HubRequestFailure failure);

}  // namespace eidolon

#endif  // EIDOLON_HOST_RESOLUTION_CORE_H_
