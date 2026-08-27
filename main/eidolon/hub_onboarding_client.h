#ifndef EIDOLON_HUB_ONBOARDING_CLIENT_H_
#define EIDOLON_HUB_ONBOARDING_CLIENT_H_

#include <esp_err.h>

#include <string>

#include "claim_recovery_core.h"
#include "device_claim_consumer_core.h"
#include "device_manifest_assertion_core.h"
#include "hub_types.h"
#include "hub_trust_store.h"

namespace eidolon {

class HubOnboardingClient {
public:
    esp_err_t Run(const AuthorityCandidateRecord& candidate,
                  const std::string& device_id,
                  Esp32HubConfig& out);
    esp_err_t Resume(const std::string& device_id, Esp32HubConfig& out);

private:
    // Loaded once per run from what commissioning left behind. Empty means this
    // device has no commissioned Owner Domain and must not accept discovery.
    esp_err_t LoadCommissionedTrust();

    esp_err_t FetchDescriptor(
        const AuthorityCandidateRecord& candidate,
        device_foundation::v1::OwnerDomainDescriptor& out);
    esp_err_t PullActiveConfiguration(const ActiveClaimState& claim,
                                      Esp32HubConfig& out);
    // Tell the Authority what this build declares, when it differs from what
    // the Authority just said it holds. Best effort on purpose: a device whose
    // assertion fails is still a working device, and the next configuration
    // poll carries the same opportunity a few seconds later.
    void ReconcileDeclaredManifest(const ActiveClaimState& claim,
                                   const AcceptedManifestRef& accepted);
    // [allow_reproposal] permits exactly one abandonment per activation
    // attempt: when the Authority answers that this Proposal is finished for
    // good, the checkpoint is dropped and a fresh Proposal is made in the same
    // pass. Bounded, so a Host that answers "finished" to everything costs one
    // extra round trip rather than an unbounded loop.
    esp_err_t ContinueCanonicalClaim(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& device_id,
        ActiveClaimState& activated_claim,
        bool& activated,
        bool allow_reproposal = true);
    esp_err_t AbandonAndRepropose(
        DeviceClaimConsumerCore& core,
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& device_id,
        ActiveClaimState& activated_claim,
        bool& activated,
        bool allow_reproposal,
        const char* reason);
    // Ask the Authority whether the Owner left an instruction for this device
    // and carry it out. `fenced` reports that a removal completed, so the
    // operational runtime must not start.
    esp_err_t ConsultOwnerInstruction(const ActiveClaimState& claim,
                                      bool& fenced);
    // Every path that knows this Claim was revoked ends here, so the Owner's
    // erase instruction is collected on all of them.
    esp_err_t StandDownRevoked(const ActiveClaimState& claim,
                               Esp32HubConfig& out);
    // Resume or start a Proposal and report where it landed. A Proposal that is
    // not yet granted is pending-approval, not a failure.
    esp_err_t ProposeFreshClaim(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& device_id, ActiveClaimState& claim,
        Esp32HubConfig& out);
    static void LogDeadClaim(
        ClaimUsability usability, const ActiveClaimState& claim,
        const std::string& device_id,
        const device_foundation::v1::OwnerDomainDescriptor& descriptor);
    esp_err_t RunAccepted(const device_foundation::v1::OwnerDomainDescriptor& descriptor,
                          const std::string& device_id,
                          Esp32HubConfig& out);

    OwnerTrustBundle trust_;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_ONBOARDING_CLIENT_H_
