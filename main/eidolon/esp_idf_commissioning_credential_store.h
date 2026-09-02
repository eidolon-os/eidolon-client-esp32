#ifndef EIDOLON_ESP_IDF_COMMISSIONING_CREDENTIAL_STORE_H_
#define EIDOLON_ESP_IDF_COMMISSIONING_CREDENTIAL_STORE_H_

#include "commissioning_credential.h"
#include "owner_trust_commissioner.h"

namespace eidolon {

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
