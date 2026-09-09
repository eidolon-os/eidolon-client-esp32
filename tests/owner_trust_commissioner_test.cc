#include <cassert>
#include <functional>
#include <string>

#include "eidolon/owner_trust_commissioner.h"

namespace {

using eidolon::OwnerTrustBundle;
using eidolon::OwnerTrustCommissioner;
using eidolon::OwnerTrustCommissioningCode;
using eidolon::CommissioningCredential;
using eidolon::CommissioningCredentialStorePort;
using eidolon::OwnerTrustStorePort;
using eidolon::OwnerTrustStoreResult;
using eidolon::OwnerTrustVerifierPort;

constexpr const char* kCertificateInJson =
    "-----BEGIN CERTIFICATE-----\\nMIIBdummy\\n-----END CERTIFICATE-----\\n";

std::string Descriptor(const std::string& owner = "owner-domain_01")
{
    return std::string("{\"owner_domain_id\":\"") + owner +
           "\",\"owner_domain_generation\":1,\"directory_revision\":7,"
           "\"descriptor_uri\":\"https://host-a.owner.test/api/device-onboarding/v1/descriptor\","
           "\"trust_root_refs\":[\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"],"
           "\"endpoints\":[{\"authority\":\"admission\","
           "\"logical_audience\":\"eidolon-admission\","
           "\"uri\":\"https://host-a.owner.test/api/admission/v1\","
           "\"transport_profile\":\"https-json\",\"priority\":10}],"
           "\"issued_at\":\"2026-08-18T00:00:00Z\","
           "\"expires_at\":\"2027-08-19T00:00:00Z\","
           "\"signing_key_id\":\"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
           "\"signature\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
}

// The voucher a Host signs during a commissioning a person is present for.
// Taken from the contract vector rather than shaped here: what this test is
// for is that the device stores what the Host actually sends.
constexpr const char* kVoucher =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJiYXNlX2lkZW50aXR5X3Byb3ZlbmFuY2UiOiJtaW50ZWQiLCJkZXZpY2VfYmFzZV9pZCI6ImRldmljZS1iYXNlLTRmM2I0ZjNiNGYzYjRmM2I0ZjNiNGYzYjRmM2I0ZjNiNGYzYjRmM2I0ZjNiNGYzYjRmM2I0ZjNiNGYzYjRmM2IiLCJleHAiOjE3ODgwMDAwMDAsImp0aSI6Imp0aS0wZjNhOTFjNGQyNWI0N2U4YTYwMzFmN2M4YjlkMmU1MCIsIm9wZXJhdGlvbmFsX3Nwa2lfc2hhMjU2Ijoic2hhMjU2OjQxMDM3NmM5ZDVkYzg4MDIyZDA0YjRmMzFiMWUwMzU0NTNiYTBjMjIyNjg4N2UwMTlmYjMzYTIzZGNhMmNiYzciLCJvd25lcl9kb21haW5faWQiOiJvd25lci1kb21haW5fMDEiLCJwdXJwb3NlIjoiZWlkb2xvbi1jb21taXNzaW9uaW5nLXZvdWNoZXItdjEifQ.YIf3fLCusRuqO7zKksG7gJYsa8rzZF4RIzOUBo45HZY";
constexpr const char* kDeviceBaseId =
    "device-base-4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b4f3b";

std::string Handover(
    const std::string& envelope_owner = "owner-domain_01",
    const std::string& descriptor_owner = "owner-domain_01",
    const std::string& voucher = kVoucher)
{
    const std::string credential =
        voucher.empty()
            ? std::string()
            : ",\"commissioning_voucher\":\"" + voucher + "\"";
    return std::string("{\"contract_version\":\"1\",\"owner_domain_id\":\"") +
           envelope_owner + "\",\"owner_domain_descriptor\":" +
           Descriptor(descriptor_owner) +
           ",\"owner_root_certificate\":\"" + kCertificateInJson +
           "\",\"authority_signing_certificate\":\"" +
           kCertificateInJson + "\"" + credential + "}";
}

class FakeCredentials final : public CommissioningCredentialStorePort {
public:
    bool accepted = true;
    int calls = 0;
    CommissioningCredential stored;

    bool Prepare(const std::string&, uint64_t, uint32_t, bool replacement,
                 eidolon::PreparedCommissioningIdentity& out) override {
        replaced = replacement;
        out = {"device-instance-410376c9d5dc88022d04b4f31b1e035453ba0c2226887e019fb33a23dca2cbc7",
               "sha256:410376c9d5dc88022d04b4f31b1e035453ba0c2226887e019fb33a23dca2cbc7"};
        return accepted;
    }
    bool replaced = false;
    bool Stage(const CommissioningCredential* candidate, uint32_t,
               const std::function<bool()>& guard) override {
        if (!guard()) return false;
        if (!candidate) return accepted;
        const auto& credential = *candidate;
        ++calls;
        if (!accepted) return false;
        stored = credential;
        return true;
    }
};

class FakeVerifier final : public OwnerTrustVerifierPort {
public:
    bool accepted = true;
    int calls = 0;
    std::function<void()> on_verify;

    bool Verify(
        const eidolon::device_foundation::v1::OwnerDomainDescriptor&,
        const std::string&,
        const std::string&,
        const std::string&) override
    {
        ++calls;
        if (on_verify) on_verify();
        return accepted;
    }
};

class FakeStore final : public OwnerTrustStorePort {
public:
    OwnerTrustBundle active;
    eidolon::OwnerTrustLoadResult ReadActive(OwnerTrustBundle& out) const override {
        out = active;
        return active.owner_domain_id.empty() ? eidolon::OwnerTrustLoadResult::NotFound
            : eidolon::OwnerTrustLoadResult::Loaded;
    }
    OwnerTrustStoreResult result = OwnerTrustStoreResult::Staged;
    int calls = 0;
    uint32_t staged_generation = 0;
    OwnerTrustBundle stored;

    OwnerTrustStoreResult Stage(
        const OwnerTrustBundle& bundle,
        uint32_t setup_generation,
        const std::function<bool()>& commit_guard) override
    {
        ++calls;
        if (!commit_guard()) return OwnerTrustStoreResult::Stale;
        stored = bundle;
        staged_generation = setup_generation;
        return result;
    }
};

void AcceptsOnlyAfterVerifyAndDurableStore()
{
    FakeVerifier verifier;
    FakeStore store;
    FakeCredentials credentials;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    const auto result = commissioner.Commission(Handover(), 7, [] { return true; });
    assert(result.code == OwnerTrustCommissioningCode::Staged);
    assert(result.owner_domain_id == "owner-domain_01");
    assert(verifier.calls == 1);
    assert(store.calls == 1);
    assert(store.stored.owner_domain_id == "owner-domain_01");
    assert(store.staged_generation == 7);
    assert(store.stored.owner_domain_descriptor_json == Descriptor());
    // Standing and trust arrive together, and standing is stored first: a
    // device that trusted an Owner Domain but could not introduce itself to it
    // would sit silent, and the only symptom would be an approval queue that
    // never shows it.
    assert(credentials.calls == 1);
    assert(credentials.stored.device_base_id == kDeviceBaseId);
    assert(credentials.stored.voucher == kVoucher);
}

void ReCommissioningWithoutAVoucherLeavesTheIdentityAlone()
{
    // Pointing an already known Body at a new network changes nothing about
    // who it is, so no voucher is sent and none is stored.
    FakeVerifier verifier;
    FakeStore store;
    FakeCredentials credentials;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    const auto result = commissioner.Commission(
        Handover("owner-domain_01", "owner-domain_01", ""), 7, [] { return true; });
    assert(result.code == OwnerTrustCommissioningCode::Staged);
    assert(credentials.calls == 0);
    assert(store.calls == 1);
}

void RefusesToTrustAnOwnerItCannotIntroduceItselfTo()
{
    FakeVerifier verifier;
    FakeStore store;
    FakeCredentials credentials;
    credentials.accepted = false;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    assert(commissioner.Commission(Handover(), 7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::StorageUnavailable);
    assert(store.calls == 0);
}

void RefusesAVoucherItCannotRead()
{
    FakeVerifier verifier;
    FakeStore store;
    FakeCredentials credentials;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    assert(commissioner.Commission(
               Handover("owner-domain_01", "owner-domain_01", "not-a-voucher"),
               7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::Invalid);
    assert(credentials.calls == 0);
    assert(store.calls == 0);
}

void RejectsMalformedAndCrossOwnerBundlesBeforeCrypto()
{
    FakeVerifier verifier;
    FakeStore store;
    FakeCredentials credentials;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    assert(commissioner.Commission("{}", 7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::Unsupported);
    assert(commissioner.Commission(
               Handover("owner-domain_01", "owner-other"), 7,
               [] { return true; }).code ==
           OwnerTrustCommissioningCode::Invalid);
    assert(verifier.calls == 0);
    assert(store.calls == 0);
}

void RejectsInvalidSignatureWithoutWriting()
{
    FakeVerifier verifier;
    verifier.accepted = false;
    FakeStore store;
    FakeCredentials credentials;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    assert(commissioner.Commission(Handover(), 7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::Invalid);
    assert(verifier.calls == 1);
    assert(store.calls == 0);
}

void GenerationFenceAppliesBeforeWorkAndAtCommit()
{
    FakeVerifier verifier;
    FakeStore store;
    FakeCredentials credentials;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    bool current = false;
    assert(commissioner.Commission(Handover(), 7, [&] { return current; }).code ==
           OwnerTrustCommissioningCode::Stale);
    assert(verifier.calls == 0);
    assert(store.calls == 0);

    current = true;
    verifier.on_verify = [&] { current = false; };
    assert(commissioner.Commission(Handover(), 7, [&] { return current; }).code ==
           OwnerTrustCommissioningCode::Stale);
    assert(verifier.calls == 1);
    assert(store.calls == 0);
}

void StorageFailureIsNotReportedAsAccepted()
{
    FakeVerifier verifier;
    FakeStore store;
    store.result = OwnerTrustStoreResult::Unavailable;
    FakeCredentials credentials;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    assert(commissioner.Commission(Handover(), 7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::StorageUnavailable);
}


void PreparePreservesTrustAndEnforcesSameOwnerRevisionFloor() {
    FakeVerifier verifier;
    FakeStore store;
    FakeCredentials credentials;
    OwnerTrustCommissioner commissioner(verifier, store, credentials);
    store.active = {"owner-domain_01", Descriptor(), "root", "authority"};
    auto prepare = Handover("owner-domain_01", "owner-domain_01", "");
    prepare.insert(1, "\"prepare_only\":true,");
    const auto result = commissioner.Commission(prepare, 9, [] { return true; });
    assert(result.code == OwnerTrustCommissioningCode::Prepared);
    assert(!result.identity.device_instance_id.empty());
    assert(store.calls == 0 && credentials.calls == 0 && !credentials.replaced);
    auto revision = [&](const std::string& text, const std::string& from, const std::string& to) {
        auto changed = text;
        changed.replace(changed.find(from), from.size(), to);
        return changed;
    };
    assert(commissioner.Commission(revision(prepare, "\"directory_revision\":7", "\"directory_revision\":6"), 9, [] { return true; }).code == OwnerTrustCommissioningCode::Invalid);
    assert(commissioner.Commission(revision(prepare, "host-a.owner.test", "host-b.owner.test"), 9, [] { return true; }).code == OwnerTrustCommissioningCode::Invalid);
    assert(commissioner.Commission(revision(prepare, "\"directory_revision\":7", "\"directory_revision\":8"), 9, [] { return true; }).code == OwnerTrustCommissioningCode::Prepared);
    assert(!credentials.replaced);
    assert(commissioner.Commission(revision(prepare, "\"owner_domain_generation\":1", "\"owner_domain_generation\":2"), 9, [] { return true; }).code == OwnerTrustCommissioningCode::Prepared);
    assert(credentials.replaced);
}


}  // namespace

int main()
{
    PreparePreservesTrustAndEnforcesSameOwnerRevisionFloor();
    AcceptsOnlyAfterVerifyAndDurableStore();
    ReCommissioningWithoutAVoucherLeavesTheIdentityAlone();
    RefusesToTrustAnOwnerItCannotIntroduceItselfTo();
    RefusesAVoucherItCannotRead();
    RejectsMalformedAndCrossOwnerBundlesBeforeCrypto();
    RejectsInvalidSignatureWithoutWriting();
    GenerationFenceAppliesBeforeWorkAndAtCommit();
    StorageFailureIsNotReportedAsAccepted();
    return 0;
}
