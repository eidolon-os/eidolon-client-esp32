#ifndef EIDOLON_DEVICE_MANIFEST_ASSERTION_CORE_H_
#define EIDOLON_DEVICE_MANIFEST_ASSERTION_CORE_H_

#include <string>

namespace eidolon {

// What the Authority currently holds as this device's own declaration, as it
// reported in the last configuration answer.
struct AcceptedManifestRef {
    // False when the answer carried no Manifest at all — an Authority that has
    // not told this device what it accepted, rather than one that accepted
    // nothing. The two must not be confused: the second invites a guess.
    bool known = false;
    std::string digest;
    int revision = 0;
};

struct ManifestAssertionPlan {
    bool assert_now = false;
    int revision = 0;
};

// Decide whether this build has something to tell the Authority about itself.
//
// A Manifest is frozen nowhere: it is what this firmware declares, and a build
// that declares something different from what the Authority holds is the only
// party that knows so. The revision asserted is always the one after the
// revision the Authority just reported, because a revision that does not move
// forward is refused — and a device guessing at that number is how a device
// with a stale Manifest stays stale.
ManifestAssertionPlan PlanManifestAssertion(
    const AcceptedManifestRef& accepted,
    const std::string& built_digest);

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_MANIFEST_ASSERTION_CORE_H_
