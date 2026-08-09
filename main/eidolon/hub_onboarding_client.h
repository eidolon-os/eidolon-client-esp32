#ifndef EIDOLON_HUB_ONBOARDING_CLIENT_H_
#define EIDOLON_HUB_ONBOARDING_CLIENT_H_

#include <esp_err.h>

#include <string>

#include "hub_types.h"

namespace eidolon {

class HubOnboardingClient {
public:
    esp_err_t Run(const HubTxtRecord& advertised,
                  const std::string& device_id,
                  Esp32HubConfig& out);
    esp_err_t Resume(const std::string& descriptor_uri,
                     const std::string& device_id,
                     Esp32HubConfig& out);

private:
    esp_err_t FetchDescriptor(const HubTxtRecord& advertised, HubDescriptor& out);
    esp_err_t EnsureEnrollment(const HubDescriptor& descriptor,
                               HubOnboardingState& state);
    esp_err_t Handoff(const HubDescriptor& descriptor,
                      HubOnboardingState& state,
                      Esp32HubConfig& out);
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_ONBOARDING_CLIENT_H_
