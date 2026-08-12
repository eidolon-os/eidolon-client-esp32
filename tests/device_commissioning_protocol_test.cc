#include <cassert>
#include <string>

#include "eidolon/device_commissioning_protocol.h"

namespace {

using eidolon::CommissioningIntent;
using eidolon::IsCommissionableCertificate;
using eidolon::IsCommissionedHub;
using eidolon::ParseCommissioningIntent;

// The same certificate twice: as it travels inside JSON, and as it arrives
// once the escapes are resolved.
const char* kCertificateInJson =
    "-----BEGIN CERTIFICATE-----\\nMIIBdummy\\n-----END CERTIFICATE-----\\n";
const char* kCertificate =
    "-----BEGIN CERTIFICATE-----\nMIIBdummy\n-----END CERTIFICATE-----\n";

std::string Payload(const std::string& fields)
{
    return std::string("{\"schema_version\":1,\"hub_id\":\"eidolon-hub-abc\",") +
           "\"hub_certificate\":\"" + kCertificateInJson + "\"" +
           (fields.empty() ? "" : "," + fields) + "}";
}

void CommissionsTrustAndNetworkTogether()
{
    CommissioningIntent intent;
    assert(ParseCommissioningIntent(
        Payload("\"wifi\":{\"ssid\":\"home\",\"password\":\"secret\"}"), intent));
    assert(intent.hub_id == "eidolon-hub-abc");
    assert(intent.certificate_pem == kCertificate);
    assert(intent.changes_network);
    assert(intent.ssid == "home");
    assert(intent.password == "secret");
}

void CommissionsTrustWithoutTouchingTheNetwork()
{
    // A device already on the right network still has to be told which Host it
    // belongs to — after the Host is rebuilt, or when it was set up before
    // there was one.
    CommissioningIntent intent;
    assert(ParseCommissioningIntent(Payload(""), intent));
    assert(!intent.changes_network);
    assert(intent.ssid.empty());
}

void AcceptsAnOpenNetwork()
{
    CommissioningIntent intent;
    assert(ParseCommissioningIntent(Payload("\"wifi\":{\"ssid\":\"cafe\"}"), intent));
    assert(intent.changes_network);
    assert(intent.password.empty());
}

void RefusesAPayloadThatNamesNoHost()
{
    CommissioningIntent intent;
    assert(!ParseCommissioningIntent(
        std::string("{\"schema_version\":1,\"hub_certificate\":\"") + kCertificateInJson + "\"}",
        intent));
    assert(!ParseCommissioningIntent(
        "{\"schema_version\":1,\"hub_id\":\"eidolon-hub-abc\"}", intent));
}

void RefusesAnythingThatIsNotACertificate()
{
    CommissioningIntent intent;
    assert(!ParseCommissioningIntent(
        "{\"schema_version\":1,\"hub_id\":\"h\",\"hub_certificate\":\"not-a-pem\"}",
        intent));
    assert(!IsCommissionableCertificate(""));
    assert(!IsCommissionableCertificate("-----BEGIN PRIVATE KEY-----"));
    assert(!IsCommissionableCertificate(
        std::string("-----BEGIN CERTIFICATE-----") + std::string(4096, 'A')));
    assert(IsCommissionableCertificate(kCertificate));
}

void RefusesAnUnusableNetwork()
{
    CommissioningIntent intent;
    assert(!ParseCommissioningIntent(Payload("\"wifi\":{\"ssid\":\"\"}"), intent));
    assert(!ParseCommissioningIntent(
        Payload("\"wifi\":{\"ssid\":\"" + std::string(33, 'n') + "\"}"), intent));
    assert(!ParseCommissioningIntent(
        Payload("\"wifi\":{\"ssid\":\"home\",\"password\":\"" + std::string(65, 'p') + "\"}"),
        intent));
}

void RefusesAForeignOrMalformedEnvelope()
{
    CommissioningIntent intent;
    assert(!ParseCommissioningIntent("", intent));
    assert(!ParseCommissioningIntent("not json", intent));
    assert(!ParseCommissioningIntent("[]", intent));
    // A future schema is not something this firmware may guess at.
    assert(!ParseCommissioningIntent(
        std::string("{\"schema_version\":2,\"hub_id\":\"h\",\"hub_certificate\":\"") +
            kCertificateInJson + "\"}",
        intent));
}

void LeavesTheIntentUntouchedWhenItRefuses()
{
    CommissioningIntent intent;
    assert(ParseCommissioningIntent(Payload(""), intent));
    assert(!ParseCommissioningIntent("garbage", intent));
    // A refused payload must not leave the previous Host half-applied.
    assert(intent.hub_id.empty());
    assert(intent.certificate_pem.empty());
}

void OnlyTheCommissionedHubIsOurs()
{
    assert(IsCommissionedHub("eidolon-hub-abc", "eidolon-hub-abc"));
    assert(!IsCommissionedHub("eidolon-hub-abc", "eidolon-hub-other"));
    // An uncommissioned device belongs to nobody, so it matches nothing —
    // including another uncommissioned answer.
    assert(!IsCommissionedHub("", "eidolon-hub-abc"));
    assert(!IsCommissionedHub("", ""));
}

}  // namespace

int main()
{
    CommissionsTrustAndNetworkTogether();
    CommissionsTrustWithoutTouchingTheNetwork();
    AcceptsAnOpenNetwork();
    RefusesAPayloadThatNamesNoHost();
    RefusesAnythingThatIsNotACertificate();
    RefusesAnUnusableNetwork();
    RefusesAForeignOrMalformedEnvelope();
    LeavesTheIntentUntouchedWhenItRefuses();
    OnlyTheCommissionedHubIsOurs();
    return 0;
}
