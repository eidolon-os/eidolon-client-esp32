#ifndef EIDOLON_HUB_ONBOARDING_CLIENT_H_
#define EIDOLON_HUB_ONBOARDING_CLIENT_H_

#include <esp_err.h>

#include <string>

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
    // device belongs to no Host yet and must not speak to one.
    esp_err_t LoadCommissionedTrust();

    esp_err_t FetchDescriptor(
        const AuthorityCandidateRecord& candidate,
        device_foundation::v1::OwnerDomainDescriptor& out,
        std::string& raw);
    esp_err_t EnsureEnrollment(
                               const device_foundation::v1::OwnerDomainDescriptor& descriptor,
                               HubOnboardingState& state);
    esp_err_t Handoff(const device_foundation::v1::OwnerDomainDescriptor& descriptor,
                      HubOnboardingState& state,
                      Esp32HubConfig& out);
    esp_err_t RunAccepted(const device_foundation::v1::OwnerDomainDescriptor& descriptor,
                          const std::string& device_id,
                          Esp32HubConfig& out);

    OwnerTrustBundle trust_;
    device_foundation::v1::OwnerDomainDescriptor accepted_descriptor_;
    std::string accepted_canonical_;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_ONBOARDING_CLIENT_H_
