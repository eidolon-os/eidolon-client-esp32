#ifndef EIDOLON_HUB_CONFIG_STORE_H_
#define EIDOLON_HUB_CONFIG_STORE_H_

#include <esp_err.h>

#include "hub_types.h"

namespace eidolon {

class HubConfigStore {
public:
    esp_err_t SaveHubConfig(const Esp32HubConfig& config);
    esp_err_t SaveOnboardingState(const HubOnboardingState& state);
    bool LoadOnboardingState(HubOnboardingState& state) const;
    void ClearOnboardingState();
    esp_err_t SaveActiveClaim(const ActiveClaimState& state);
    bool LoadActiveClaim(ActiveClaimState& state) const;
    void ClearActiveClaim();
    bool HasValidConfig() const;
    bool Load(Esp32HubConfig& config) const;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_CONFIG_STORE_H_
