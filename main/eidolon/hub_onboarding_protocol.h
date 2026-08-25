#ifndef EIDOLON_HUB_ONBOARDING_PROTOCOL_H_
#define EIDOLON_HUB_ONBOARDING_PROTOCOL_H_

#include <string>

#include "device_claim_consumer_core.h"
#include "device_manifest_assertion_core.h"
#include "hub_types.h"

namespace eidolon {

bool ParseOwnerDomainDescriptor(
    const std::string& body,
    device_foundation::v1::OwnerDomainDescriptor& out,
    std::string& canonical_signing_bytes);

std::string BuildDeviceManifestJson(const std::string& board_name, bool has_camera);

bool ParseDeviceConfigurationResponse(
    const std::string& body,
    const std::string& expected_nonce,
    const ActiveClaimState& expected,
    HubConfigStatus& status,
    HubChannelAssignment& assignment,
    AcceptedManifestRef& accepted_manifest);


bool ParseLiveKitBinding(const std::string& body, Esp32HubConfig& out);

// Does this Authority answer mean the Proposal is finished for good?
//
// Keyed on the canonical problem code, never on the status alone. A 404 can
// equally mean the request reached an origin that does not own the route, and
// "the Proposal is gone" is not a conclusion to draw from a wrong address —
// which is exactly the mistake a client makes when it treats a bare 404 as
// state. A Proposal that is finished can never produce a Claim, so the device
// drops its checkpoint and proposes again for review.
bool IsFinishedProposalProblem(int status, const std::string& body);

// The canonical SPKI string the Admission Claim records for a device's
// operational key, built from its bare base64 DER.
//
// Distinct from the bare base64 the pre-canonical signed-request header
// carries, and not interchangeable with it: the Authority compares this string
// exactly, so presenting the other form is a rejection with no other symptom.
std::string CanonicalOperationalPublicKeySpki(const std::string& public_key_base64);

}  // namespace eidolon

#endif  // EIDOLON_HUB_ONBOARDING_PROTOCOL_H_
