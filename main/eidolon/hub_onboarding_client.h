#ifndef EIDOLON_HUB_ONBOARDING_CLIENT_H_
#define EIDOLON_HUB_ONBOARDING_CLIENT_H_

#include <esp_err.h>

#include <string>

#include "device_claim_consumer_core.h"
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
    esp_err_t ContinueCanonicalClaim(
        const device_foundation::v1::OwnerDomainDescriptor& descriptor,
        const std::string& device_id,
        ActiveClaimState& activated_claim,
        bool& activated);
    esp_err_t RunAccepted(const device_foundation::v1::OwnerDomainDescriptor& descriptor,
                          const std::string& device_id,
                          Esp32HubConfig& out);

    OwnerTrustBundle trust_;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_ONBOARDING_CLIENT_H_
