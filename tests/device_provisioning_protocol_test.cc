#include <cassert>
#include <string>

#include "eidolon/device_provisioning_protocol.h"

namespace {

using eidolon::BuildEnrollmentReceiptJson;
using eidolon::BuildProvisioningDescriptorJson;
using eidolon::BuildTrustAcceptedJson;
using eidolon::BuildTrustRefusedJson;
using eidolon::IsCommissionableCertificate;
using eidolon::IsCommissionedHub;
using eidolon::ParseTrustHandover;
using eidolon::ProvisioningDescriptor;
using eidolon::TrustHandover;

// The same certificate twice: as it travels inside JSON, and as it arrives once
// the escapes are resolved.
const char* kCertificateInJson =
    "-----BEGIN CERTIFICATE-----\\nMIIBdummy\\n-----END CERTIFICATE-----\\n";
const char* kCertificate =
    "-----BEGIN CERTIFICATE-----\nMIIBdummy\n-----END CERTIFICATE-----\n";

std::string Handover(const std::string& hub_id = "ehost-0123456789abcdef0123")
{
    return std::string("{\"contract_version\":\"1\",\"hub_id\":\"") + hub_id +
           "\",\"hub_certificate\":\"" + kCertificateInJson + "\"}";
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

ProvisioningDescriptor SampleDescriptor()
{
    ProvisioningDescriptor descriptor;
    descriptor.device_id = "aa:bb:cc:dd:ee:ff";
    descriptor.device_kind = "atk-dnesp32s3";
    descriptor.display_name = "atk-dnesp32s3";
    descriptor.identity_fingerprint = "sha256:abcdef";
    descriptor.session_id = "sess-0001";
    descriptor.expires_in_seconds = 600;
    return descriptor;
}

void DescribesThisDeviceToAController()
{
    const std::string body = BuildProvisioningDescriptorJson(SampleDescriptor());
    assert(Contains(body, "\"contract_version\":\"1\""));
    assert(Contains(body, "\"device_id\":\"aa:bb:cc:dd:ee:ff\""));
    assert(Contains(body, "\"device_kind\":\"atk-dnesp32s3\""));
    assert(Contains(body, "\"identity_fingerprint\":\"sha256:abcdef\""));
    assert(Contains(body, "\"session_id\":\"sess-0001\""));
}

void SaysHowLongTheWindowLastsRatherThanWhenItEnds()
{
    // A device being set up has not joined a network and has no wall clock, so
    // an absolute expiry would be a number it cannot honestly produce.
    const std::string body = BuildProvisioningDescriptorJson(SampleDescriptor());
    assert(Contains(body, "\"expires_in_seconds\":600"));
    assert(!Contains(body, "expires_at"));
}

void DeclaresDevelopmentAndProductionTrustAsOneField()
{
    // Only the value differs between a development device and a production one.
    // If these two ever stop being the same act, it will be visible here first.
    ProvisioningDescriptor development = SampleDescriptor();
    ProvisioningDescriptor production = SampleDescriptor();
    production.manufacturer_bound = true;
    assert(Contains(BuildProvisioningDescriptorJson(development), "\"trust\":\"development-tofu\""));
    assert(Contains(BuildProvisioningDescriptorJson(production), "\"trust\":\"manufacturer-bound\""));
}

void AcceptsTheHostAControllerHandsOver()
{
    TrustHandover handover;
    assert(ParseTrustHandover(Handover(), handover));
    assert(handover.hub_id == "ehost-0123456789abcdef0123");
    assert(handover.certificate_pem == kCertificate);
}

void RefusesAHandoverThatNamesNoHost()
{
    TrustHandover handover;
    assert(!ParseTrustHandover(
        std::string("{\"contract_version\":\"1\",\"hub_certificate\":\"") + kCertificateInJson +
            "\"}",
        handover));
    assert(!ParseTrustHandover(
        "{\"contract_version\":\"1\",\"hub_id\":\"ehost-abc\"}", handover));
}

void RefusesAnythingThatIsNotACertificate()
{
    TrustHandover handover;
    assert(!ParseTrustHandover(
        "{\"contract_version\":\"1\",\"hub_id\":\"h\",\"hub_certificate\":\"not-a-pem\"}",
        handover));
    assert(!IsCommissionableCertificate(""));
    assert(!IsCommissionableCertificate("-----BEGIN PRIVATE KEY-----"));
    assert(!IsCommissionableCertificate(
        std::string("-----BEGIN CERTIFICATE-----") + std::string(4096, 'A')));
    assert(IsCommissionableCertificate(kCertificate));
}

void RefusesAnOversizedHostId()
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
    assert(!ParseTrustHandover(
        std::string("{\"contract_version\":\"2\",\"hub_id\":\"h\",\"hub_certificate\":\"") +
            kCertificateInJson + "\"}",
        handover));
    // Nor an unversioned one: every Eidolon contract carries its version.
    assert(!ParseTrustHandover(
        std::string("{\"hub_id\":\"h\",\"hub_certificate\":\"") + kCertificateInJson + "\"}",
        handover));
}

void LeavesTheHandoverUntouchedWhenItRefuses()
{
    TrustHandover handover;
    assert(ParseTrustHandover(Handover(), handover));
    assert(!ParseTrustHandover("garbage", handover));
    // A refused payload must not leave the previous Host half-applied.
    assert(handover.hub_id.empty());
    assert(handover.certificate_pem.empty());
}

void OnlyTheCommissionedHubIsOurs()
{
    assert(IsCommissionedHub("ehost-abc", "ehost-abc"));
    assert(!IsCommissionedHub("ehost-abc", "ehost-other"));
    // A device that belongs to nobody matches nothing — including another
    // uncommissioned answer.
    assert(!IsCommissionedHub("", "ehost-abc"));
    assert(!IsCommissionedHub("", ""));
}

void AnswersATrustHandoverWithoutClaimingMore()
{
    const std::string accepted = BuildTrustAcceptedJson("aa:bb", "ehost-abc");
    assert(Contains(accepted, "\"accepted\":true"));
    assert(Contains(accepted, "\"hub_id\":\"ehost-abc\""));
    // Accepting the Host is not joining a network and not being admitted by it.
    assert(!Contains(accepted, "network"));
    assert(!Contains(accepted, "lifecycle"));

    const std::string refused = BuildTrustRefusedJson("payload is not supported");
    assert(Contains(refused, "\"accepted\":false"));
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

}  // namespace

int main()
{
    DescribesThisDeviceToAController();
    SaysHowLongTheWindowLastsRatherThanWhenItEnds();
    DeclaresDevelopmentAndProductionTrustAsOneField();
    AcceptsTheHostAControllerHandsOver();
    RefusesAHandoverThatNamesNoHost();
    RefusesAnythingThatIsNotACertificate();
    RefusesAnOversizedHostId();
    RefusesAForeignOrMalformedEnvelope();
    LeavesTheHandoverUntouchedWhenItRefuses();
    OnlyTheCommissionedHubIsOurs();
    AnswersATrustHandoverWithoutClaimingMore();
    ReportsAMissingEnrollmentAsAnAnswer();
    return 0;
}
