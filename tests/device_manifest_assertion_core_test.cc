#include "eidolon/device_manifest_assertion_core.h"

#include <cassert>
#include <cstdio>
#include <string>

using eidolon::AcceptedManifestRef;
using eidolon::ManifestAssertionPlan;
using eidolon::PlanManifestAssertion;

namespace {

const std::string kBuilt = "sha256:aaaa";
const std::string kOther = "sha256:bbbb";

void AgreementIsSilent()
{
    AcceptedManifestRef accepted{true, kBuilt, 4};
    const ManifestAssertionPlan plan = PlanManifestAssertion(accepted, kBuilt);
    assert(!plan.assert_now);
}

void DisagreementAssertsAfterWhatTheAuthorityHolds()
{
    AcceptedManifestRef accepted{true, kOther, 4};
    const ManifestAssertionPlan plan = PlanManifestAssertion(accepted, kBuilt);
    assert(plan.assert_now);
    // Not 1, and not a guess: exactly one past the revision just reported. A
    // device that reuses the reported revision for different content is refused,
    // which is how a device with a stale Manifest stays stale forever.
    assert(plan.revision == 5);
}

void AnUnreportedManifestIsNotAnAbsentOne()
{
    // An Authority that said nothing about the Manifest has not said the device
    // has none. Asserting here would be asserting at a number nobody supplied.
    AcceptedManifestRef silent{false, "", 0};
    assert(!PlanManifestAssertion(silent, kBuilt).assert_now);

    AcceptedManifestRef incoherent{true, kOther, 0};
    assert(!PlanManifestAssertion(incoherent, kBuilt).assert_now);
}

void ABuildThatCannotDescribeItselfSaysNothing()
{
    AcceptedManifestRef accepted{true, kOther, 4};
    assert(!PlanManifestAssertion(accepted, "").assert_now);
}

void TheStuckDeviceHeals()
{
    // The case this exists for. A device enrolled declaring a placeholder, so
    // the Authority holds revision 1 of a document this build does not agree
    // with, and the Channel Provider has been provisioning from it ever since.
    AcceptedManifestRef placeholder{true, "sha256:placeholder", 1};
    const ManifestAssertionPlan plan = PlanManifestAssertion(placeholder, kBuilt);
    assert(plan.assert_now);
    assert(plan.revision == 2);

    // And once accepted, it settles: no further assertion, no assertion loop.
    AcceptedManifestRef settled{true, kBuilt, 2};
    assert(!PlanManifestAssertion(settled, kBuilt).assert_now);
}

}  // namespace

int main()
{
    AgreementIsSilent();
    DisagreementAssertsAfterWhatTheAuthorityHolds();
    AnUnreportedManifestIsNotAnAbsentOne();
    ABuildThatCannotDescribeItselfSaysNothing();
    TheStuckDeviceHeals();
    std::printf("device_manifest_assertion_core: all assertions passed\n");
    return 0;
}
