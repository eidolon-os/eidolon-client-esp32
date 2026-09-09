#ifndef EIDOLON_HUB_TRUST_STORE_H_
#define EIDOLON_HUB_TRUST_STORE_H_

#include <string>
#include <cstdint>

#include <esp_err.h>

#include "owner_trust_commissioner.h"

namespace eidolon {

// Commissioning is the only writer of the Owner trust anchor. Discovery may
// later update a signed directory, but can never replace these certificates.
class OwnerTrustStore : public OwnerTrustStorePort {
public:
    OwnerTrustLoadResult ReadActive(OwnerTrustBundle& out) const override;
    bool DiscardInactive();
    OwnerTrustStoreResult Stage(
        const OwnerTrustBundle& bundle,
        uint32_t setup_generation,
        const std::function<bool()>& commit_guard) override;
    bool LoadStaged(uint32_t setup_generation, OwnerTrustBundle& bundle) const;
    OwnerTrustStoreResult CommitStaged(
        uint32_t setup_generation,
        const std::function<bool()>& commit_guard);
    OwnerTrustStoreResult RollbackStaged(uint32_t setup_generation);
    bool Load(OwnerTrustBundle& bundle) const;
    std::string CommissionedOwnerDomainId() const;
    esp_err_t SaveAcceptedDescriptor(const std::string& descriptor_json);
    void Clear();

private:
    OwnerTrustStoreResult ReplaceActive(
        const OwnerTrustBundle& bundle,
        const std::function<bool()>& commit_guard);
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_TRUST_STORE_H_
