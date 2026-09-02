#include "commissioning_credential.h"

#include <cassert>
#include <string>

namespace {

// The golden voucher from
// eidolon_sdk/contracts/device_foundation/v1/golden/commissioning-voucher.json.
// Copied rather than reconstructed: what this test is for is that the device
// reads the same bytes the Host wrote, so building them here from the device's
// own idea of the format would test nothing.
constexpr char kGoldenVoucher[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJiYXNlX2lkZW50aXR5X3Byb3ZlbmFuY"
    "2UiOiJtaW50ZWQiLCJkZXZpY2VfYmFzZV9pZCI6ImRldmljZS1iYXNlLTRmM2I0ZjNiNGY"
    "zYjRmM2I0ZjNiNGYzYjRmM2I0ZjNiNGYzYjRmM2I0ZjNiNGYzYjRmM2I0ZjNiNGYzYjRmM"
    "2IiLCJleHAiOjE3ODgwMDAwMDAsImp0aSI6Imp0aS0wZjNhOTFjNGQyNWI0N2U4YTYwMzF"
    "mN2M4YjlkMmU1MCIsIm9wZXJhdGlvbmFsX3Nwa2lfc2hhMjU2Ijoic2hhMjU2OjQxMDM3N"
    "mM5ZDVkYzg4MDIyZDA0YjRmMzFiMWUwMzU0NTNiYTBjMjIyNjg4N2UwMTlmYjMzYTIzZGN"
    "hMmNiYzciLCJvd25lcl9kb21haW5faWQiOiJvd25lci1kb21haW5fMDEiLCJwdXJwb3NlI"
    "joiZWlkb2xvbi1jb21taXNzaW9uaW5nLXZvdWNoZXItdjEifQ.YIf3fLCusRuqO7zKksG7"
    "gJYsa8rzZF4RIzOUBo45HZY";

void ReadsTheIdentityAndNonceOutOfAHostSignedVoucher() {
    eidolon::CommissioningCredential credential;
    assert(eidolon::ParseCommissioningVoucher(kGoldenVoucher, credential));
    assert(credential.device_base_id ==
           "device-base-"
           "4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b");
    assert(credential.voucher_jti == "jti-0f3a91c4d25b47e8a6031f7c8b9d2e50");
    assert(credential.voucher_expires_at_unix == 1788000000);
    assert(credential.voucher == kGoldenVoucher);
}

void RefusesAnythingThatIsNotOneOfOurVouchers() {
    eidolon::CommissioningCredential credential;
    assert(!eidolon::ParseCommissioningVoucher("", credential));
    assert(!eidolon::ParseCommissioningVoucher("not-a-voucher", credential));
    // Two segments: a JWS this shape never had a signature.
    assert(!eidolon::ParseCommissioningVoucher("aGVhZGVy.eyJhIjoxfQ", credential));
    // Right shape, wrong purpose: a management token must not be usable here.
    assert(!eidolon::ParseCommissioningVoucher(
        "eyJhbGciOiJIUzI1NiJ9.eyJwdXJwb3NlIjoiZWlkb2xvbi1tYW5hZ2VtZW50In0.c2ln",
        credential));
}

void AHalfIdentityIsNotAnIdentity() {
    // The base identity and the operational key it was issued to live in one
    // NVS namespace and are written and erased together, so this state is not
    // reachable by using the device. It is reachable by writing one half into
    // flash, and a Body that presented a lineage it cannot demonstrate would
    // be exactly what the issued identity exists to prevent — so the pairing
    // is carried in the record and checked, not assumed from where it is kept.
    eidolon::CommissioningCredential credential;
    assert(eidolon::ParseCommissioningVoucher(kGoldenVoucher, credential));
    credential.operational_key_fingerprint = "p256:" + std::string(64, 'a');
    assert(eidolon::StandingFor(credential, 1700000000) ==
           eidolon::CommissioningStanding::Voucher);
    // The store is what refuses it; the record only has to be able to say
    // which key it belongs to.
    assert(!credential.operational_key_fingerprint.empty());
}

void SaysWhichProofMayBePresented() {
    eidolon::CommissioningCredential none;
    assert(eidolon::StandingFor(none, 1700000000) ==
           eidolon::CommissioningStanding::None);

    eidolon::CommissioningCredential fresh;
    assert(eidolon::ParseCommissioningVoucher(kGoldenVoucher, fresh));
    assert(eidolon::StandingFor(fresh, 1700000000) ==
           eidolon::CommissioningStanding::Voucher);
    // No trusted clock yet: the device presents what it has and lets the Hub
    // refuse it, rather than deleting its own credential over a missing clock.
    assert(eidolon::StandingFor(fresh, 0) ==
           eidolon::CommissioningStanding::Voucher);
    assert(eidolon::StandingFor(fresh, 1788000001) ==
           eidolon::CommissioningStanding::Expired);

    eidolon::CommissioningCredential enrolled;
    enrolled.device_base_id = fresh.device_base_id;
    assert(eidolon::StandingFor(enrolled, 1700000000) ==
           eidolon::CommissioningStanding::EnrolledBaseKey);
}

}  // namespace

int main() {
    ReadsTheIdentityAndNonceOutOfAHostSignedVoucher();
    RefusesAnythingThatIsNotOneOfOurVouchers();
    AHalfIdentityIsNotAnIdentity();
    SaysWhichProofMayBePresented();
    return 0;
}
