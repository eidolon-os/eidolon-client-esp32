#ifndef EIDOLON_COMMISSIONING_CREDENTIAL_H_
#define EIDOLON_COMMISSIONING_CREDENTIAL_H_

#include <cstdint>
#include <string>

namespace eidolon {

// What a Body holds between being commissioned and being admitted.
//
// This device ships with no identity material of any kind: one firmware image
// is valid for every unit, and nothing in it says which unit this is. What
// makes a first Proposal possible is a one-shot voucher the Host signed during
// a commissioning a person was present for, naming a base identity the Hub
// minted and binding it to the operational key this device generated for
// itself. The voucher is worth nothing to anything holding a different key.
//
// The base identity outlives the voucher: it is the anchor a removed Body comes
// back on, and it must be stored in the same place as the operational key it is
// bound to, so that erasing one erases the other. "Base identity present, key
// gone" is not a state this device may be in — it would be a Body that cannot
// prove it is itself but still claims a lineage.
struct CommissioningCredential {
    std::string device_base_id;
    std::string owner_domain_id;
    std::string operational_key_id;
    uint64_t owner_domain_generation = 0;
    std::string voucher;      // empty once spent or expired
    std::string voucher_jti;  // the nonce a voucher-backed Proposal must carry
    int64_t voucher_expires_at_unix = 0;
};

// Read the claims out of a Host-signed voucher without verifying its signature.
//
// Verification belongs to the Hub, which holds the key; the device only needs
// to know which base identity it was given and which nonce to send. Parsing is
// still strict about shape, because a malformed voucher stored now becomes an
// unexplained 401 much later, on a different day, with nothing left to inspect.
bool ParseCommissioningVoucher(const std::string& voucher,
                               CommissioningCredential& out);

// Which proof a Body may present right now.
enum class CommissioningStanding {
    None,           // nothing was ever issued: this Body must be commissioned
    Voucher,        // a first Proposal, witnessed by a Controller
    EnrolledBaseKey,  // continuing a lifecycle this Owner Domain already knows
    Expired,        // a voucher was issued and ran out before the Host was reached
};

// Decide from what is stored, not from what the caller hopes. `now_unix` may be
// zero when the device has no trusted clock yet: an unexpired-looking voucher is
// then preferred over silence, and the Hub is the one that refuses it — the
// device must not delete its own credential because it cannot read a clock.
CommissioningStanding StandingFor(const CommissioningCredential& credential,
                                  int64_t now_unix);

}  // namespace eidolon

#endif  // EIDOLON_COMMISSIONING_CREDENTIAL_H_
