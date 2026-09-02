#ifndef EIDOLON_ESP_IDF_COMMISSIONING_CREDENTIAL_STORE_H_
#define EIDOLON_ESP_IDF_COMMISSIONING_CREDENTIAL_STORE_H_

#include "commissioning_credential.h"
#include "owner_trust_commissioner.h"

namespace eidolon {

// Where this store keeps what it keeps, published because two other operations
// have to end this identity completely: the Owner's remote erase, and the
// physical recovery that follows a completed one. Both used to name the
// operational private key and stop there, which left a device holding a base
// identity issued to a key it no longer had — the half identity B3 declares
// unrepresentable. It cost one Body a 401 loop it could not leave.
//
// Naming the keys here, once, is what makes the colocation below load-bearing
// rather than decorative: a key added to this store is erased by both
// operations without either being edited.
inline constexpr const char* kCommissioningCredentialNamespace = "eidolon_id";
inline constexpr const char* kCommissioningCredentialKeys[] = {
    "base_id", "voucher", "voucher_jti", "voucher_exp"};

// The credential lives in the same NVS namespace as the operational private
// key, on purpose. Erasing that namespace erases both, which is the whole of
// what "this device was reset" now means: no factory secret survives it, so the
// Body that comes back is a new one and the Owner is asked about it as such.
class EspIdfCommissioningCredentialStore : public CommissioningCredentialStorePort {
public:
    static EspIdfCommissioningCredentialStore& GetInstance();

    bool Save(const CommissioningCredential& credential) override;
    bool Load(CommissioningCredential& out) const;

    // Called once a voucher has been exchanged for a Proposal. The base identity
    // stays: it is the anchor this Body returns on. Only the one-shot half is
    // dropped, so a spent voucher cannot be presented again from flash.
    bool ForgetSpentVoucher();
};

}  // namespace eidolon

#endif  // EIDOLON_ESP_IDF_COMMISSIONING_CREDENTIAL_STORE_H_
