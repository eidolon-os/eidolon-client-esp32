#include <cassert>
#include <functional>
#include <string>

#include "eidolon/owner_trust_commissioner.h"

namespace {

using eidolon::OwnerTrustBundle;
using eidolon::OwnerTrustCommissioner;
using eidolon::OwnerTrustCommissioningCode;
using eidolon::OwnerTrustStorePort;
using eidolon::OwnerTrustStoreResult;
using eidolon::OwnerTrustVerifierPort;

constexpr const char* kCertificateInJson =
    "-----BEGIN CERTIFICATE-----\\nMIIBdummy\\n-----END CERTIFICATE-----\\n";

std::string Descriptor(const std::string& owner = "owner_01")
{
    return std::string("{\"owner_domain_id\":\"") + owner +
           "\",\"owner_domain_generation\":1,\"directory_revision\":7,"
           "\"trust_root_refs\":[\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"],"
           "\"endpoints\":[{\"authority\":\"admission\","
           "\"logical_audience\":\"eidolon-admission\","
           "\"uri\":\"https://host-a.owner.test/api/device-onboarding/v1\","
           "\"transport_profile\":\"https-json\",\"priority\":10}],"
           "\"issued_at\":\"2026-08-18T00:00:00Z\","
           "\"expires_at\":\"2027-08-19T00:00:00Z\","
           "\"signing_key_id\":\"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
           "\"signature\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
}

std::string Handover(const std::string& envelope_owner = "owner_01",
                     const std::string& descriptor_owner = "owner_01")
{
    return std::string("{\"contract_version\":\"1\",\"owner_domain_id\":\"") +
           envelope_owner + "\",\"owner_domain_descriptor\":" +
           Descriptor(descriptor_owner) +
           ",\"owner_root_certificate\":\"" + kCertificateInJson +
           "\",\"authority_signing_certificate\":\"" +
           kCertificateInJson + "\"}";
}

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
    OwnerTrustCommissioner commissioner(verifier, store);
    const auto result = commissioner.Commission(Handover(), 7, [] { return true; });
    assert(result.code == OwnerTrustCommissioningCode::Staged);
    assert(result.owner_domain_id == "owner_01");
    assert(verifier.calls == 1);
    assert(store.calls == 1);
    assert(store.stored.owner_domain_id == "owner_01");
    assert(store.staged_generation == 7);
    assert(store.stored.owner_domain_descriptor_json == Descriptor());
}

void RejectsMalformedAndCrossOwnerBundlesBeforeCrypto()
{
    FakeVerifier verifier;
    FakeStore store;
    OwnerTrustCommissioner commissioner(verifier, store);
    assert(commissioner.Commission("{}", 7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::Unsupported);
    assert(commissioner.Commission(
               Handover("owner_01", "owner_other"), 7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::Invalid);
    assert(verifier.calls == 0);
    assert(store.calls == 0);
}

void RejectsInvalidSignatureWithoutWriting()
{
    FakeVerifier verifier;
    verifier.accepted = false;
    FakeStore store;
    OwnerTrustCommissioner commissioner(verifier, store);
    assert(commissioner.Commission(Handover(), 7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::Invalid);
    assert(verifier.calls == 1);
    assert(store.calls == 0);
}

void GenerationFenceAppliesBeforeWorkAndAtCommit()
{
    FakeVerifier verifier;
    FakeStore store;
    OwnerTrustCommissioner commissioner(verifier, store);
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
    assert(store.calls == 1);
}

void StorageFailureIsNotReportedAsAccepted()
{
    FakeVerifier verifier;
    FakeStore store;
    store.result = OwnerTrustStoreResult::Unavailable;
    OwnerTrustCommissioner commissioner(verifier, store);
    assert(commissioner.Commission(Handover(), 7, [] { return true; }).code ==
           OwnerTrustCommissioningCode::StorageUnavailable);
}

}  // namespace

int main()
{
    AcceptsOnlyAfterVerifyAndDurableStore();
    RejectsMalformedAndCrossOwnerBundlesBeforeCrypto();
    RejectsInvalidSignatureWithoutWriting();
    GenerationFenceAppliesBeforeWorkAndAtCommit();
    StorageFailureIsNotReportedAsAccepted();
    return 0;
}
