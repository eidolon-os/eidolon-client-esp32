#pragma once

#include "commissioning_credential.h"
#include "owner_trust_commissioner.h"

namespace eidolon {
inline constexpr const char* kCommissioningCredentialNamespace = "eidolon_id";
inline constexpr const char* kCommissioningCredentialKeys[] = {
    "base_id", "voucher", "voucher_jti", "voucher_exp", "id_active", "id_pending"};

enum class CommissioningIdentityLoad { NotFound, Loaded, Unavailable };

// One active key/credential blob and one transaction candidate. The private key
// and issued base identity are published by a single NVS item replacement.
class EspIdfCommissioningCredentialStore : public CommissioningCredentialStorePort {
public:
    static EspIdfCommissioningCredentialStore& GetInstance();
    bool Prepare(const std::string& owner, uint64_t owner_generation,
                 uint32_t setup_generation, bool replace_identity,
                 PreparedCommissioningIdentity& out) override;
    bool Stage(const CommissioningCredential* credential, uint32_t generation,
               const std::function<bool()>& guard) override;
    bool DescribeCandidate(uint32_t generation, PreparedCommissioningIdentity& out) const;
    bool StagedDigest(uint32_t generation, const std::string& owner, uint64_t owner_generation,
                      std::string& digest, bool& replaces_owner) const;
    bool RequiresRuntimeRestart(uint32_t generation) const;
    bool CommitStaged(uint32_t generation, const std::string& digest);
    bool Finish(uint32_t generation);
    bool Rollback(uint32_t generation);
    bool Load(CommissioningCredential& out) const;
    bool ForgetSpentVoucher();
    static CommissioningIdentityLoad LoadPrivateKey(std::string& pem);
};
}  // namespace eidolon
