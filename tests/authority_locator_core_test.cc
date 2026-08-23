#include <cassert>
#include <string>
#include <vector>

#include "eidolon/authority_locator_core.h"

namespace {

using eidolon::AuthorityLocatorCore;
using eidolon::AuthorityLocatorResult;
using eidolon::device_foundation::v1::AuthorityEndpoint;
using eidolon::device_foundation::v1::LogicalAuthority;
using eidolon::device_foundation::v1::OwnerDomainDescriptor;

std::string Canonical(const OwnerDomainDescriptor& descriptor)
{
    std::string value = descriptor.owner_domain_id + "|" +
                        std::to_string(descriptor.owner_domain_generation) + "|" +
                        std::to_string(descriptor.directory_revision);
    for (const auto& endpoint : descriptor.endpoints) {
        value += "|" + std::to_string(static_cast<int>(endpoint.authority)) +
                 "|" + endpoint.logical_audience + "|" + endpoint.uri + "|" +
                 endpoint.transport_profile + "|" +
                 std::to_string(endpoint.priority);
    }
    return value;
}

class StrictVerifier final : public eidolon::AuthorityDescriptorVerifierPort {
public:
    bool Verify(const OwnerDomainDescriptor& descriptor,
                const std::string& canonical) const override
    {
        return signature_valid && descriptor.signature == "valid-signature" &&
               canonical == Canonical(descriptor);
    }

    bool IsUsableNow(const OwnerDomainDescriptor&) const override
    {
        return current;
    }

    bool signature_valid = true;
    bool current = true;
};

class RecordingStore final : public eidolon::AuthorityDescriptorStorePort {
public:
    bool SaveAcceptedDescriptor(const std::string& descriptor_json) override
    {
        ++writes;
        last = descriptor_json;
        return available;
    }

    bool available = true;
    int writes = 0;
    std::string last;
};

OwnerDomainDescriptor Descriptor(uint64_t revision, const std::string& host,
                                 uint16_t priority = 10,
                                 uint64_t owner_generation = 1)
{
    OwnerDomainDescriptor value;
    value.owner_domain_id = "owner_01";
    value.owner_domain_generation = owner_generation;
    value.directory_revision = revision;
    value.signature = "valid-signature";
    value.endpoints = {
        AuthorityEndpoint{LogicalAuthority::Admission, "eidolon-admission",
                          "https://" + host + "/admission", "https-json",
                          priority},
        AuthorityEndpoint{LogicalAuthority::DeviceControl,
                          "eidolon-device-control",
                          "https://" + host + "/device-control", "https-json",
                          priority},
    };
    return value;
}

void Restore(AuthorityLocatorCore& locator,
             const OwnerDomainDescriptor& descriptor)
{
    assert(locator.Restore(descriptor, Canonical(descriptor)) ==
           AuthorityLocatorResult::Accepted);
}

void TestDfHost001RelocationChangesOnlyResolvedEndpoint()
{
    StrictVerifier verifier;
    RecordingStore store;
    AuthorityLocatorCore locator(verifier, store);
    locator.Commission("owner_01");
    const auto host_a = Descriptor(7, "host-a.owner.test");
    Restore(locator, host_a);
    const std::string device_identity = "device-instance-01";
    const auto host_b = Descriptor(8, "host-b.owner.test");
    assert(locator.Accept(host_b, Canonical(host_b), "signed-host-b") ==
           AuthorityLocatorResult::Accepted);

    std::vector<const AuthorityEndpoint*> routes;
    assert(locator.Resolve("owner_01", LogicalAuthority::Admission, routes) ==
           AuthorityLocatorResult::Accepted);
    assert(routes.front()->uri == "https://host-b.owner.test/admission");
    assert(device_identity == "device-instance-01");
    assert(store.writes == 1 && store.last == "signed-host-b");
}

void TestDfHost002And008RevisionRulesFailClosed()
{
    StrictVerifier verifier;
    RecordingStore store;
    AuthorityLocatorCore locator(verifier, store);
    locator.Commission("owner_01");
    const auto current = Descriptor(8, "active.owner.test");
    Restore(locator, current);
    assert(locator.Accept(current, Canonical(current), "duplicate") ==
           AuthorityLocatorResult::Unchanged);
    assert(store.writes == 0);

    auto conflict = Descriptor(8, "other.owner.test");
    assert(locator.Accept(conflict, Canonical(conflict), "conflict") ==
           AuthorityLocatorResult::RevisionConflict);
    const auto retired = Descriptor(7, "retired.owner.test");
    assert(locator.Accept(retired, Canonical(retired), "rollback") ==
           AuthorityLocatorResult::RevisionRollback);

    std::vector<const AuthorityEndpoint*> routes;
    assert(locator.Resolve("owner_01", LogicalAuthority::Admission, routes) ==
           AuthorityLocatorResult::Accepted);
    assert(routes.front()->uri == "https://active.owner.test/admission");
    assert(store.writes == 0);
}

void TestDfHost004TrustAndOwnerAreNotDiscoveryInputs()
{
    StrictVerifier verifier;
    RecordingStore store;
    AuthorityLocatorCore locator(verifier, store);
    locator.Commission("owner_01");
    auto attacker = Descriptor(7, "attacker.test");
    attacker.owner_domain_id = "attacker_owner";
    assert(locator.Accept(attacker, Canonical(attacker), "attacker") ==
           AuthorityLocatorResult::WrongOwnerDomain);
    verifier.signature_valid = false;
    const auto forged = Descriptor(7, "forged.test");
    assert(locator.Accept(forged, Canonical(forged), "forged") ==
           AuthorityLocatorResult::DescriptorRejected);
    assert(locator.accepted() == nullptr && store.writes == 0);
}

void TestDfHost005SingleHostUsesLogicalAuthorityAndPriority()
{
    StrictVerifier verifier;
    RecordingStore store;
    AuthorityLocatorCore locator(verifier, store);
    locator.Commission("owner_01");
    auto descriptor = Descriptor(7, "primary.owner.test", 20);
    descriptor.endpoints.push_back(
        AuthorityEndpoint{LogicalAuthority::Admission, "eidolon-admission",
                          "https://preferred.owner.test/admission", "https-json", 5});
    Restore(locator, descriptor);
    std::vector<const AuthorityEndpoint*> routes;
    assert(locator.Resolve("owner_01", LogicalAuthority::Admission, routes) ==
           AuthorityLocatorResult::Accepted);
    assert(routes.size() == 2);
    assert(routes.front()->uri == "https://preferred.owner.test/admission");
    assert(locator.Resolve("other_owner", LogicalAuthority::Admission, routes) ==
           AuthorityLocatorResult::WrongOwnerDomain);
}

void TestCommitFailureNeverChangesVisibleDirectory()
{
    StrictVerifier verifier;
    RecordingStore store;
    AuthorityLocatorCore locator(verifier, store);
    locator.Commission("owner_01");
    const auto host_a = Descriptor(7, "host-a.owner.test");
    Restore(locator, host_a);
    store.available = false;
    const auto host_b = Descriptor(8, "host-b.owner.test");
    assert(locator.Accept(host_b, Canonical(host_b), "host-b") ==
           AuthorityLocatorResult::PersistenceFailed);
    std::vector<const AuthorityEndpoint*> routes;
    assert(locator.Resolve("owner_01", LogicalAuthority::Admission, routes) ==
           AuthorityLocatorResult::Accepted);
    assert(routes.front()->uri == "https://host-a.owner.test/admission");
}

void TestOwnerGenerationFencesRollbackAndRevisionReset()
{
    StrictVerifier verifier;
    RecordingStore store;
    AuthorityLocatorCore locator(verifier, store);
    locator.Commission("owner_01");
    const auto generation_two = Descriptor(9, "generation-two.owner.test", 10, 2);
    Restore(locator, generation_two);

    const auto retired = Descriptor(99, "retired.owner.test", 10, 1);
    assert(locator.Accept(retired, Canonical(retired), "retired") ==
           AuthorityLocatorResult::OwnerGenerationRollback);

    const auto generation_three = Descriptor(1, "generation-three.owner.test", 10, 3);
    assert(locator.Accept(generation_three, Canonical(generation_three),
                          "generation-three") ==
           AuthorityLocatorResult::OwnerGenerationAdvanced);
    assert(locator.accepted()->owner_domain_generation == 3);
    assert(locator.accepted()->directory_revision == 1);
}

void TestExpiredDirectoryRequiresDiscoveryWithoutClearingOwner()
{
    StrictVerifier verifier;
    RecordingStore store;
    AuthorityLocatorCore locator(verifier, store);
    locator.Commission("owner_01");
    const auto descriptor = Descriptor(7, "host-a.owner.test");
    Restore(locator, descriptor);
    verifier.current = false;
    std::vector<const AuthorityEndpoint*> routes;
    assert(locator.Resolve("owner_01", LogicalAuthority::Admission, routes) ==
           AuthorityLocatorResult::AuthorityDiscoveryRequired);
    assert(locator.accepted() != nullptr);
}

}  // namespace

int main()
{
    TestDfHost001RelocationChangesOnlyResolvedEndpoint();
    TestDfHost002And008RevisionRulesFailClosed();
    TestDfHost004TrustAndOwnerAreNotDiscoveryInputs();
    TestDfHost005SingleHostUsesLogicalAuthorityAndPriority();
    TestCommitFailureNeverChangesVisibleDirectory();
    TestOwnerGenerationFencesRollbackAndRevisionReset();
    TestExpiredDirectoryRequiresDiscoveryWithoutClearingOwner();
    return 0;
}
