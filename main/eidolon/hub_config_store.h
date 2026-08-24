#ifndef EIDOLON_HUB_CONFIG_STORE_H_
#define EIDOLON_HUB_CONFIG_STORE_H_

#include <esp_err.h>

#include "device_claim_consumer_core.h"
#include "hub_types.h"

namespace eidolon {

class HubConfigStore final : public EnrollmentJournalPort,
                             public ActiveClaimStorePort {
public:
    esp_err_t SaveHubConfig(const Esp32HubConfig& config);
    bool HasValidConfig() const;
    bool Load(Esp32HubConfig& config) const;

    ClaimStoreLoadResult LoadEnrollment(
        EnrollmentJournalEntry& out) override;
    bool StoreEnrollment(const EnrollmentJournalEntry& value) override;
    bool ClearEnrollment() override;
    ClaimStoreLoadResult LoadActiveClaim(ActiveClaimState& out) override;
    bool StoreActiveClaim(const ActiveClaimState& value) override;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_CONFIG_STORE_H_
