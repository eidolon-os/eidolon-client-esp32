#include "eidolon/device_manifest_assertion_core.h"

namespace eidolon {

ManifestAssertionPlan PlanManifestAssertion(
    const AcceptedManifestRef& accepted,
    const std::string& built_digest)
{
    ManifestAssertionPlan plan;
    if (built_digest.empty()) {
        // Nothing to say. A build that cannot describe itself must not claim to.
        return plan;
    }
    if (!accepted.known || accepted.revision < 1) {
        // Wait to be told. The next configuration answer carries the reference,
        // and asserting before then would be asserting at a guessed revision.
        return plan;
    }
    if (accepted.digest == built_digest) {
        // The Authority already holds what this build declares.
        return plan;
    }
    plan.assert_now = true;
    plan.revision = accepted.revision + 1;
    return plan;
}

}  // namespace eidolon
