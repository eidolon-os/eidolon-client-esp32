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

struct DeviceCapabilities;
std::string BuildDeviceManifestJson(const std::string& board_name, const DeviceCapabilities& capabilities);
std::string BuildDeviceManifestJson(const std::string& board_name, bool has_camera);

// What the DeviceRef in a configuration answer means for the one this device
// holds. The Authority finds the Claim by device identity and answers with the
// ref it holds, so a Body that was re-granted while it kept an older ref learns
// its generation here instead of being refused forever.
//
// Only the Claim's own generations move this way. Device and Owner Domain are
// who is asking and who answers; the Owner Domain generation changes only when
// the Authority is reset, and that is the descriptor's to report, through the
// recovery it requires. A re-grant restarts trust_epoch at one, so trust_epoch
// is ordered within a claim_generation, never across one.
enum class DeviceRefCorrection {
    None,    // the Authority holds exactly this ref
    Adopt,   // the Authority holds a later generation of this same Claim
    Reject,  // another device, Owner Domain or Authority generation, or older
};
DeviceRefCorrection ClassifyAuthorityDeviceRef(
    const device_foundation::v1::DeviceRef& held,
    const device_foundation::v1::DeviceRef& answered);

// On success, authority_ref is the ref the answer carries: equal to the one
// expected, or a later generation of it (DeviceRefCorrection::Adopt).
bool ParseDeviceConfigurationResponse(
    const std::string& body,
    const std::string& expected_nonce,
    const ActiveClaimState& expected,
    HubConfigStatus& status,
    HubChannelAssignment& assignment,
    AcceptedManifestRef& accepted_manifest,
    DeviceOutputPolicy* output_policy = nullptr,
    const char** rejection_reason = nullptr,
    device_foundation::v1::DeviceRef* authority_ref = nullptr,
    std::string* channel_problem_code = nullptr);


// Temporary invitation only. Parsing never writes a Claim or cached configuration.
// Caller must authenticate the sender before using this payload.
struct SharedSessionInvitation {
    std::string session_id;
    int64_t deadline_ms = 0;
    HubChannelAssignment channel;
};
bool ParseSharedSessionInvitation(const std::string& body,
                                  const ActiveClaimState& expected,
                                  int64_t now_ms,
                                  SharedSessionInvitation& out);

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
