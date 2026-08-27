#include <cassert>
#include <string>

#include "eidolon/host_resolution_core.h"

namespace {

using eidolon::HostOfUrl;
using eidolon::HubRequestFailure;
using eidolon::IsLinkLocalName;
using eidolon::ShouldDropCachedResolution;

void TheHostIsTakenFromTheUrlTheRequestActuallyUsed()
{
    assert(HostOfUrl("https://eidolon-hub-abc.local:9443/api/x") ==
           "eidolon-hub-abc.local");
    assert(HostOfUrl("https://eidolon-hub-abc.local/api/x") ==
           "eidolon-hub-abc.local");
    assert(HostOfUrl("ws://eidolon-hub-abc.local:7880") ==
           "eidolon-hub-abc.local");
    assert(HostOfUrl("https://192.168.3.206:9443/api/x") == "192.168.3.206");
    // No host to speak of, rather than a guess.
    assert(HostOfUrl("not-a-url").empty());
    assert(HostOfUrl("https://").empty());
}

void OnlyNamesThisDeviceResolvesForItselfAreLinkLocal()
{
    assert(IsLinkLocalName("eidolon-hub-abc.local"));
    assert(!IsLinkLocalName("hub.example.com"));
    assert(!IsLinkLocalName("192.168.3.206"));
    // ".local" alone is a suffix, not a name.
    assert(!IsLinkLocalName(".local"));
    assert(!IsLinkLocalName(""));
    // A name that merely contains the letters is not a link-local name.
    assert(!IsLinkLocalName("local.example.com"));
    assert(!IsLinkLocalName("my.local.host"));
}

// The regression this exists for: a wrong mDNS answer is cached for the
// answer's TTL, so every retry re-resolves through the same dead entry and the
// activation loop asks down the same wrong path until it ages out. Dropping it
// is what makes the next attempt a new attempt rather than a repeat.
void AConnectionThatNeverOpenedDropsALinkLocalAddress()
{
    assert(ShouldDropCachedResolution("eidolon-hub-abc.local",
                                      HubRequestFailure::ConnectionNotOpened));
}

// An Authority that answered has already proved the address was right. Whatever
// it answered — 401, 403, 500 — says nothing about the name, and throwing away
// a good resolution on every rejected request would be pure churn.
void AnAuthorityThatAnsweredKeepsItsAddress()
{
    assert(!ShouldDropCachedResolution("eidolon-hub-abc.local",
                                       HubRequestFailure::AuthorityAnswered));
}

// A name a real resolver owns is not this device's to second-guess, and
// clearing it buys nothing: the next lookup asks the same resolver.
void AWanNameIsLeftAlone()
{
    assert(!ShouldDropCachedResolution("hub.example.com",
                                       HubRequestFailure::ConnectionNotOpened));
    assert(!ShouldDropCachedResolution("192.168.3.206",
                                       HubRequestFailure::ConnectionNotOpened));
    // An unparsable URL yields no host, and no host is nothing to drop.
    assert(!ShouldDropCachedResolution("", HubRequestFailure::ConnectionNotOpened));
}

}  // namespace

int main()
{
    TheHostIsTakenFromTheUrlTheRequestActuallyUsed();
    OnlyNamesThisDeviceResolvesForItselfAreLinkLocal();
    AConnectionThatNeverOpenedDropsALinkLocalAddress();
    AnAuthorityThatAnsweredKeepsItsAddress();
    AWanNameIsLeftAlone();
    return 0;
}
