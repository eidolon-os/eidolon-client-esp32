#include <cJSON.h>

#include <cassert>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "eidolon/device_provisioning_protocol.h"
#include "eidolon/provisioning_window_policy_core.h"

namespace {

using eidolon::AdvertisedWindowSeconds;
using eidolon::BuildEnrollmentReceiptJson;
using eidolon::BuildSetupDescriptorJson;
using eidolon::BuildTrustStagedJson;
using eidolon::BuildTrustRefusedJson;
using eidolon::DecideProvisioningWindow;
using eidolon::IsCommissionableCertificate;
using eidolon::IsCommissionedOwnerDomain;
using eidolon::ParseTrustHandover;
using eidolon::ProvisioningWindowTrigger;
using eidolon::TrustHandover;
using eidolon::device_foundation::v1::SetupDescriptor;
using eidolon::device_foundation::v1::SetupDescriptorTrust;
using eidolon::device_foundation::v1::SetupDescriptorTrustWireValue;
using eidolon::device_foundation::v1::SetupWindowRemainingSeconds;

// The canonical setup descriptor vector, synced byte-for-byte from the SDK by
// scripts/sync_device_foundation_v1.py. The descriptor used to be a field table
// written out by hand here and a second one written out by hand in the
// controller, kept in step by a person comparing two files — which is how this
// device came to encode an endless setup window as 0 while the controller
// refused any duration it could not act on, and every device out of the box was
// told its own description broke the contract. Reading the vector rather than
// restating it is what makes a field added to the contract a red test here.
std::string ReadSetupDescriptorVector()
{
    std::ifstream file("tests/fixtures/device_foundation_v1/setup-descriptor.json");
    assert(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string VectorString(const cJSON* node, const char* key)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(node, key);
    assert(cJSON_IsString(item) && item->valuestring != nullptr);
    return item->valuestring;
}

// Fill a descriptor from the vector's own inputs. Every field has to be named
// here to be carried, so a field the contract gained and this producer has not
// is a field the built bytes are missing.
SetupDescriptor DescriptorFromVector(const cJSON* descriptor)
{
    SetupDescriptor value;
    value.device_id = VectorString(descriptor, "device_id");
    value.device_kind = VectorString(descriptor, "device_kind");
    value.display_name = VectorString(descriptor, "display_name");
    value.identity_fingerprint = VectorString(descriptor, "identity_fingerprint");
    value.session_id = VectorString(descriptor, "session_id");
    const cJSON* expires =
        cJSON_GetObjectItemCaseSensitive(descriptor, "expires_in_seconds");
    value.expires_in = cJSON_IsNumber(expires)
                           ? SetupWindowRemainingSeconds::FromPositiveSeconds(
                                 static_cast<int64_t>(expires->valuedouble))
                           : std::nullopt;
    value.trust =
        VectorString(descriptor, "trust") ==
                SetupDescriptorTrustWireValue(SetupDescriptorTrust::ManufacturerBound)
            ? SetupDescriptorTrust::ManufacturerBound
            : SetupDescriptorTrust::DevelopmentTofu;
    return value;
}

void TheDescriptorBytesAreTheGoldenVectorBytes()
{
    const std::string raw = ReadSetupDescriptorVector();
    cJSON* vector = cJSON_ParseWithLength(raw.data(), raw.size());
    assert(cJSON_IsObject(vector));
    // Both shapes of the same document: an offer that ends, and one that does
    // not. The second is the whole reason this vector exists.
    for (const char* shape : {"bounded_window", "no_deadline"}) {
        const cJSON* node = cJSON_GetObjectItemCaseSensitive(vector, shape);
        assert(cJSON_IsObject(node));
        const cJSON* descriptor = cJSON_GetObjectItemCaseSensitive(node, "descriptor");
        assert(cJSON_IsObject(descriptor));
        assert(BuildSetupDescriptorJson(DescriptorFromVector(descriptor)) ==
               VectorString(node, "canonical_utf8"));
    }
    cJSON_Delete(vector);
}

// The same certificate twice: as it travels inside JSON, and as it arrives once
// the escapes are resolved.
const char* kCertificateInJson =
    "-----BEGIN CERTIFICATE-----\\nMIIBdummy\\n-----END CERTIFICATE-----\\n";
const char* kCertificate =
    "-----BEGIN CERTIFICATE-----\nMIIBdummy\n-----END CERTIFICATE-----\n";

std::string Handover(const std::string& owner_domain_id = "owner-0123456789abcdef0123")
{
    return std::string("{\"contract_version\":\"1\",\"owner_domain_id\":\"") + owner_domain_id +
           "\",\"owner_domain_descriptor\":{\"owner_domain_id\":\"owner_01\"},"
           "\"owner_root_certificate\":\"" + kCertificateInJson +
           "\",\"authority_signing_certificate\":\"" + kCertificateInJson + "\"}";
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

SetupDescriptor SampleDescriptor()
{
    const std::string raw = ReadSetupDescriptorVector();
    cJSON* vector = cJSON_ParseWithLength(raw.data(), raw.size());
    assert(cJSON_IsObject(vector));
    SetupDescriptor descriptor = DescriptorFromVector(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(vector, "bounded_window"), "descriptor"));
    cJSON_Delete(vector);
    return descriptor;
}

void DescribesThisDeviceToAController()
{
    const std::string body = BuildSetupDescriptorJson(SampleDescriptor());
    const SetupDescriptor descriptor = SampleDescriptor();
    assert(Contains(body, "\"contract_version\":\"1\""));
    assert(Contains(body, "\"device_id\":\"" + descriptor.device_id + "\""));
    assert(Contains(body, "\"device_kind\":\"" + descriptor.device_kind + "\""));
    assert(Contains(body,
                    "\"identity_fingerprint\":\"" + descriptor.identity_fingerprint + "\""));
    assert(Contains(body, "\"session_id\":\"" + descriptor.session_id + "\""));
}

void SaysHowLongTheWindowLastsRatherThanWhenItEnds()
{
    // A device being set up has not joined a network and has no wall clock, so
    // an absolute expiry would be a number it cannot honestly produce.
    const std::string body = BuildSetupDescriptorJson(SampleDescriptor());
    assert(Contains(body, "\"expires_in_seconds\":600"));
    assert(!Contains(body, "expires_at"));
}

void AnOfferWithNoDeadlineCarriesNoDurationField()
{
    // A window that never closes has no duration, so the descriptor leaves the
    // field out entirely. It must not encode "no deadline" as a number: a
    // controller cannot tell a sentinel apart from an uninitialised one, and
    // the last sentinel we shipped here (0) made every factory device look
    // like it was advertising a window that had already closed.
    SetupDescriptor descriptor = SampleDescriptor();
    descriptor.expires_in.reset();
    const std::string body = BuildSetupDescriptorJson(descriptor);
    assert(!Contains(body, "expires_in_seconds"));
    // Everything else about the offer is still there to be acted on.
    assert(Contains(body, "\"session_id\":\"" + descriptor.session_id + "\""));
    assert(Contains(body, "\"contract_version\":\"1\""));
}

void ADurationThatIsNotOneCannotBeBuiltAtAll()
{
    // The canonical type has no room for a sentinel: there is no way to hold 0
    // or a negative number, so no producer can put one on the wire even by
    // mistake. This is the fix for the failure above expressed in the type
    // rather than in a convention.
    assert(!SetupWindowRemainingSeconds::FromPositiveSeconds(0).has_value());
    assert(!SetupWindowRemainingSeconds::FromPositiveSeconds(-1).has_value());
    const auto bounded = SetupWindowRemainingSeconds::FromPositiveSeconds(600);
    assert(bounded.has_value() && bounded->seconds() == 600);
}

void AFactoryDeviceAdvertisesAnOfferTheControllerCanAccept()
{
    // The seam this whole pair of types exists for: the window policy decides
    // whether there is a deadline at all, and only a deadline that exists
    // reaches the wire. A device that has never been commissioned keeps its
    // offer open, so its descriptor names no duration; an Owner reopening setup
    // on a commissioned device gets the bounded window it advertised.
    SetupDescriptor factory = SampleDescriptor();
    factory.expires_in = AdvertisedWindowSeconds(DecideProvisioningWindow(
        ProvisioningWindowTrigger::NeverCommissioned, 600));
    assert(!Contains(BuildSetupDescriptorJson(factory), "expires_in_seconds"));

    SetupDescriptor reopened = SampleDescriptor();
    reopened.expires_in = AdvertisedWindowSeconds(DecideProvisioningWindow(
        ProvisioningWindowTrigger::OwnerPresenceReopen, 600));
    assert(Contains(BuildSetupDescriptorJson(reopened),
                    "\"expires_in_seconds\":600"));
}

void DeclaresDevelopmentAndProductionTrustAsOneField()
{
    // Only the value differs between a development device and a production one.
    // If these two ever stop being the same act, it will be visible here first.
    SetupDescriptor development = SampleDescriptor();
    SetupDescriptor production = SampleDescriptor();
    development.trust = SetupDescriptorTrust::DevelopmentTofu;
    production.trust = SetupDescriptorTrust::ManufacturerBound;
    assert(Contains(BuildSetupDescriptorJson(development), "\"trust\":\"development-tofu\""));
    assert(Contains(BuildSetupDescriptorJson(production), "\"trust\":\"manufacturer-bound\""));
}

void AcceptsTheOwnerDomainAControllerHandsOver()
{
    TrustHandover handover;
    assert(ParseTrustHandover(Handover(), handover));
    assert(handover.owner_domain_id == "owner-0123456789abcdef0123");
    assert(handover.owner_root_certificate_pem == kCertificate);
    assert(handover.authority_signing_certificate_pem == kCertificate);
    assert(Contains(handover.owner_domain_descriptor_json, "owner_domain_id"));
}

void RefusesAHandoverThatNamesNoOwnerDomain()
{
    TrustHandover handover;
    assert(!ParseTrustHandover("{\"contract_version\":\"1\"}", handover));
    assert(!ParseTrustHandover(
        "{\"contract_version\":\"1\",\"owner_domain_id\":\"owner-abc\"}", handover));
}

void RefusesAnythingThatIsNotACertificate()
{
    TrustHandover handover;
    std::string invalid = Handover();
    const auto start = invalid.find(kCertificateInJson);
    invalid.replace(start, std::string(kCertificateInJson).size(), "not-a-pem");
    assert(!ParseTrustHandover(invalid, handover));
    assert(!IsCommissionableCertificate(""));
    assert(!IsCommissionableCertificate("-----BEGIN PRIVATE KEY-----"));
    assert(!IsCommissionableCertificate(
        std::string("-----BEGIN CERTIFICATE-----") + std::string(4096, 'A')));
    assert(IsCommissionableCertificate(kCertificate));
}

void RefusesAnOversizedOwnerDomainId()
{
    TrustHandover handover;
    assert(!ParseTrustHandover(Handover(std::string(129, 'h')), handover));
}

void RefusesAForeignOrMalformedEnvelope()
{
    TrustHandover handover;
    assert(!ParseTrustHandover("", handover));
    assert(!ParseTrustHandover("not json", handover));
    assert(!ParseTrustHandover("[]", handover));
    // A future contract version is not something this firmware may guess at.
    std::string future = Handover();
    future.replace(future.find("\"1\""), 3, "\"2\"");
    assert(!ParseTrustHandover(future, handover));
    // Nor an unversioned one: every Eidolon contract carries its version.
    assert(!ParseTrustHandover("{\"owner_domain_id\":\"h\"}", handover));
}

void LeavesTheHandoverUntouchedWhenItRefuses()
{
    TrustHandover handover;
    assert(ParseTrustHandover(Handover(), handover));
    assert(!ParseTrustHandover("garbage", handover));
    // A refused payload must not leave the previous Owner Domain half-applied.
    assert(handover.owner_domain_id.empty());
    assert(handover.owner_root_certificate_pem.empty());
    assert(handover.authority_signing_certificate_pem.empty());
}

void OnlyTheCommissionedOwnerDomainIsOurs()
{
    assert(IsCommissionedOwnerDomain("owner-abc", "owner-abc"));
    assert(!IsCommissionedOwnerDomain("owner-abc", "owner-other"));
    // A device that belongs to nobody matches nothing — including another
    // uncommissioned answer.
    assert(!IsCommissionedOwnerDomain("", "owner-abc"));
    assert(!IsCommissionedOwnerDomain("", ""));
}

void AnswersATrustHandoverWithoutClaimingMore()
{
    const std::string staged = BuildTrustStagedJson("aa:bb", "owner-abc");
    assert(Contains(staged, "\"staged\":true"));
    assert(!Contains(staged, "\"accepted\""));
    assert(Contains(staged, "\"owner_domain_id\":\"owner-abc\""));
    // Staging the Owner Domain is not joining a network or activating trust.
    assert(!Contains(staged, "network"));
    assert(!Contains(staged, "lifecycle"));

    const std::string refused = BuildTrustRefusedJson("payload is not supported");
    assert(Contains(refused, "\"staged\":false"));
    assert(Contains(refused, "payload is not supported"));
}

void ReportsAMissingEnrollmentAsAnAnswer()
{
    // The controller asking before this device has enrolled is expected, not an
    // error: it will ask again.
    const std::string pending = BuildEnrollmentReceiptJson("aa:bb", "", "");
    assert(Contains(pending, "\"enrolled\":false"));
    assert(!Contains(pending, "lifecycle_state"));

    const std::string ready =
        BuildEnrollmentReceiptJson("aa:bb", "enroll-7", "pending-approval");
    assert(Contains(ready, "\"enrolled\":true"));
    assert(Contains(ready, "\"enrollment_id\":\"enroll-7\""));
    assert(Contains(ready, "\"lifecycle_state\":\"pending-approval\""));
}

void EmitsOnlyCompleteCanonicalCommissioningSuccess()
{
    using namespace eidolon::device_foundation::v1;
    CommissioningStatusEvidence evidence;
    evidence.session_id = "setup_session_01";
    evidence.setup_generation = 7;
    evidence.state_revision = 5;
    evidence.state = CommissioningStatusState::Committed;
    evidence.conditions = {true, true, true, true};
    const std::string body = eidolon::BuildCommissioningStatusJson(evidence);
    assert(Contains(body, "\"state\":\"committed\""));
    assert(Contains(body, "\"owner_route_validated\":true"));
    assert(Contains(body, "\"failure_code\":null"));

    evidence.conditions.owner_route_validated = false;
    assert(eidolon::BuildCommissioningStatusJson(evidence).empty());
}

void TerminalAckIsStrictAndGenerationBound()
{
    using namespace eidolon::device_foundation::v1;
    const std::string body =
        "{\"contract\":\"eidolon.device-foundation.commissioning-terminal-ack\","
        "\"contract_version\":\"1.0\",\"session_id\":\"setup_session_01\","
        "\"setup_generation\":7,\"observed_state_revision\":5}";
    CommissioningTerminalAck ack;
    assert(eidolon::ParseCommissioningTerminalAck(body, ack));
    assert(ack.session_id == "setup_session_01");
    assert(ack.setup_generation == 7);
    assert(ack.observed_state_revision == 5);
    assert(!eidolon::ParseCommissioningTerminalAck(
        body.substr(0, body.size() - 1) + ",\"extra\":true}", ack));
}

}  // namespace

int main()
{
    TheDescriptorBytesAreTheGoldenVectorBytes();
    DescribesThisDeviceToAController();
    SaysHowLongTheWindowLastsRatherThanWhenItEnds();
    AnOfferWithNoDeadlineCarriesNoDurationField();
    ADurationThatIsNotOneCannotBeBuiltAtAll();
    AFactoryDeviceAdvertisesAnOfferTheControllerCanAccept();
    DeclaresDevelopmentAndProductionTrustAsOneField();
    AcceptsTheOwnerDomainAControllerHandsOver();
    RefusesAHandoverThatNamesNoOwnerDomain();
    RefusesAnythingThatIsNotACertificate();
    RefusesAnOversizedOwnerDomainId();
    RefusesAForeignOrMalformedEnvelope();
    LeavesTheHandoverUntouchedWhenItRefuses();
    OnlyTheCommissionedOwnerDomainIsOurs();
    AnswersATrustHandoverWithoutClaimingMore();
    ReportsAMissingEnrollmentAsAnAnswer();
    EmitsOnlyCompleteCanonicalCommissioningSuccess();
    TerminalAckIsStrictAndGenerationBound();
    return 0;
}
