#include <cJSON.h>

#include <cassert>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "eidolon/hub_onboarding_protocol.h"
#include "eidolon/hub_txt_parser.h"

namespace {

// The Device Foundation golden vector, synced byte-for-byte from the SDK by
// scripts/sync_device_foundation_v1.py. Tests read it rather than restating it:
// an inlined copy of the canonical signing bytes is a second authority, and the
// two drifted the moment a descriptor field was added — signature verification
// would break on device with this suite still green.
std::string ReadGoldenVector()
{
    std::ifstream file(
        "tests/fixtures/device_foundation_v1/owner-domain-descriptor.json");
    assert(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

using eidolon::HubConfigStatus;

eidolon::AuthorityCandidateRecord Advertised()
{
    eidolon::AuthorityCandidateRecord value;
    value.txtvers = 1;
    value.owner_domain_id = "owner-domain_01";
    value.owner_domain_descriptor_uri =
        "https://eidolon-hub.local/api/device-onboarding/v1/descriptor";
    return value;
}

void TestMdnsConsumesOnlyDescriptorContract()
{
    eidolon::AuthorityCandidateRecord record;
    assert(eidolon::HubTxtParser::Parse(
               {{"txtvers", "1"},
                {"owner_domain_id", Advertised().owner_domain_id},
                {"owner_domain_descriptor_uri",
                 Advertised().owner_domain_descriptor_uri}},
               record) == ESP_OK);
    assert(record.owner_domain_id == Advertised().owner_domain_id);
    assert(eidolon::HubTxtParser::Parse(
               {{"txtvers", "1"},
                {"api", "v1"},
                {"register_url", "https://eidolon-hub.local/api/device/register"}},
               record) == ESP_ERR_INVALID_RESPONSE);
    assert(eidolon::HubTxtParser::Parse(
               {{"txtvers", "1"},
                {"owner_domain_id", "owner-domain_01"},
                {"owner_domain_descriptor_uri",
                 "http://eidolon-hub.local/descriptor"}},
               record) == ESP_ERR_INVALID_RESPONSE);
}

void TestDescriptorCanonicalisationMatchesTheGoldenVector()
{
    cJSON* vector = cJSON_Parse(ReadGoldenVector().c_str());
    assert(vector != nullptr);
    const cJSON* document = cJSON_GetObjectItemCaseSensitive(vector, "descriptor");
    char* encoded = cJSON_PrintUnformatted(document);
    assert(encoded != nullptr);
    const std::string descriptor(encoded);
    cJSON_free(encoded);
    const cJSON* expected =
        cJSON_GetObjectItemCaseSensitive(vector, "canonical_signing_utf8");
    assert(cJSON_IsString(expected));
    const std::string expected_canonical(expected->valuestring);
    cJSON_Delete(vector);

    eidolon::device_foundation::v1::OwnerDomainDescriptor parsed;
    std::string canonical;
    assert(eidolon::ParseOwnerDomainDescriptor(descriptor, parsed, canonical));
    assert(canonical == expected_canonical);
    assert(parsed.owner_domain_id == "owner-domain_01");
    assert(parsed.owner_domain_generation == 3);
    assert(parsed.endpoints.size() == 2);
    assert(parsed.endpoints.front().authority ==
           eidolon::device_foundation::v1::LogicalAuthority::Admission);
    // The document says where it is published; the device never derives that
    // from an endpoint base address.
    assert(parsed.descriptor_uri ==
           "https://host-a.owner.test/api/device-onboarding/v1/descriptor");
    assert(parsed.descriptor_uri != parsed.endpoints.front().uri + "/descriptor");
}

void TestDescriptorWithoutItsOwnRouteIsRejected()
{
    // A document that does not say where it is published cannot be re-fetched on
    // the Owner route, so accepting it would only move the failure later, to a
    // request built from a guess.
    const std::string descriptor =
        "{\"owner_domain_id\":\"owner-domain_01\",\"owner_domain_generation\":3,"
        "\"directory_revision\":7,"
        "\"trust_root_refs\":[\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"],"
        "\"endpoints\":[{\"authority\":\"admission\","
        "\"logical_audience\":\"eidolon-admission\","
        "\"uri\":\"https://host-a.owner.test/api/admission/v1\","
        "\"transport_profile\":\"https-json\",\"priority\":10}],"
        "\"issued_at\":\"2026-08-18T00:00:00Z\","
        "\"expires_at\":\"2026-08-19T00:00:00Z\","
        "\"signing_key_id\":\"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
        "\"signature\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
    eidolon::device_foundation::v1::OwnerDomainDescriptor parsed;
    std::string canonical;
    assert(!eidolon::ParseOwnerDomainDescriptor(descriptor, parsed, canonical));

    const std::string plaintext_route =
        descriptor.substr(0, descriptor.size() - 1) +
        ",\"descriptor_uri\":\"http://host-a.owner.test/api/device-onboarding/v1/descriptor\"}";
    assert(!eidolon::ParseOwnerDomainDescriptor(plaintext_route, parsed, canonical));
}

void TestCanonicalManifest()
{
    const std::string manifest =
        eidolon::BuildDeviceManifestJson("esp32-s3-touch-amoled-2.06", /*has_camera=*/false);
    // A device states its turn-taking as an immutable property, so the Provider
    // reads a fact rather than guessing one. The value follows this build's
    // compile-time profile — half duplex here, pinned by tests/stubs/sdkconfig.h.
    assert(manifest ==
           "{\"actions\":[],\"events\":[],\"media\":[{\"codecs\":[\"opus\"],"
           "\"direction\":\"bidirectional\",\"kind\":\"audio\"}],"
           "\"properties\":[{\"name\":\"interaction_mode\",\"observable\":false,"
           "\"schema\":{\"const\":\"half_duplex\",\"type\":\"string\"},"
           "\"writable\":false}],"
           "\"schema_version\":1,\"title\":\"esp32-s3-touch-amoled-2.06\"}");
}

void TestActiveClaimConfigurationAndProviderBinding()
{
    HubConfigStatus status = HubConfigStatus::Active;
    eidolon::HubChannelAssignment assignment;
    eidolon::device_foundation::v1::DeviceRef device_ref;
    device_ref.device_instance_id = "aa:bb";
    device_ref.owner_domain_id.value = "owner-domain_01";
    device_ref.owner_domain_generation = 3;
    device_ref.claim_generation = 1;
    device_ref.trust_epoch = 1;
    eidolon::ActiveClaimState active_claim;
    active_claim.device_ref = device_ref;
    active_claim.manifest_ref = {
        "manifest_01", 1,
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    active_claim.grant_id = "grant_01";
    const std::string configuration =
        "{\"operation\":\"device-control.configuration\","
        "\"nonce\":\"configuration-nonce\",\"device_ref\":{"
        "\"device_instance_id\":\"aa:bb\",\"owner_domain_id\":\"owner-domain_01\","
        "\"owner_domain_generation\":3,\"claim_generation\":1,"
        "\"trust_epoch\":1},"
        "\"lifecycle_state\":\"approved\",\"channels\":[{"
        "\"channel_id\":\"livekit-1\",\"purpose\":\"voice\","
        "\"kinds\":[\"audio\"],\"binding_format\":"
        "\"application/vnd.eidolon.livekit-session+json;v=2\","
        "\"issued_at_ms\":1,\"expires_at_ms\":1786000000000,"
        "\"opaque_binding\":\"e30=\"}]}";
    eidolon::AcceptedManifestRef accepted_manifest;
    assert(eidolon::ParseDeviceConfigurationResponse(
        configuration, "configuration-nonce", active_claim, status,
        assignment, accepted_manifest));
    // An answer that says nothing about the Manifest says nothing: it is not a
    // statement that the Authority holds none, and must not read as one.
    assert(!accepted_manifest.known);

    // When it does report one, that is what the device measures itself against.
    const std::string with_manifest =
        "{\"operation\":\"device-control.configuration\","
        "\"nonce\":\"configuration-nonce\",\"device_ref\":{"
        "\"device_instance_id\":\"aa:bb\",\"owner_domain_id\":\"owner-domain_01\","
        "\"owner_domain_generation\":3,\"claim_generation\":1,"
        "\"trust_epoch\":1},"
        "\"lifecycle_state\":\"approved\",\"manifest\":{"
        "\"manifest_id\":\"esp-box-3\",\"revision\":4,\"digest\":"
        "\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"},"
        "\"channels\":[]}";
    assert(eidolon::ParseDeviceConfigurationResponse(
        with_manifest, "configuration-nonce", active_claim, status,
        assignment, accepted_manifest));
    assert(accepted_manifest.known);
    assert(accepted_manifest.revision == 4);
    assert(accepted_manifest.digest ==
           "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    assert(status == eidolon::HubConfigStatus::WaitingBinding);

    const std::string binding =
        "{\"schema_version\":2,\"session\":{\"server_url\":\"wss://lk\","
        "\"token\":\"tok\",\"identity\":\"device\",\"room_name\":\"channel\"},"
        "\"audio\":{\"sample_rate\":16000,\"channels\":1}}";
    eidolon::Esp32HubConfig config;
    assert(eidolon::ParseLiveKitBinding(binding, config));
    assert(config.session.room_name == "channel");
    assert(config.session.token == "tok");

    // The Provider issues one channel. A device that accepted the old pair
    // would connect to rooms nobody serves, so the shape is refused outright
    // rather than half-read.
    const std::string two_rooms =
        "{\"schema_version\":1,\"active\":{\"server_url\":\"wss://lk\","
        "\"token\":\"voice\",\"identity\":\"device\",\"room_name\":\"voice\"},"
        "\"control\":{\"server_url\":\"wss://lk\",\"token\":\"control\","
        "\"identity\":\"device\",\"room_name\":\"control\"},"
        "\"audio\":{\"sample_rate\":16000,\"channels\":1}}";
    eidolon::Esp32HubConfig stale;
    assert(!eidolon::ParseLiveKitBinding(two_rooms, stale));
}

void TestFinishedProposalIsRecognizedOnlyFromTheAuthoritysOwnWords()
{
    const auto problem = [](const char* code, const char* authority) {
        return std::string("{\"code\":\"") + code +
               "\",\"category\":\"expired\",\"retryable\":false," +
               "\"authority\":\"" + authority + "\",\"detail\":\"gone\"," +
               "\"incident_id\":\"incident_01\"}";
    };

    // These three, and only from Admission, end a Proposal.
    assert(eidolon::IsFinishedProposalProblem(
        410, problem("PROPOSAL_EXPIRED", "admission")));
    assert(eidolon::IsFinishedProposalProblem(
        410, problem("GRANT_EXPIRED", "admission")));
    assert(eidolon::IsFinishedProposalProblem(
        404, problem("NOT_FOUND", "admission")));

    // A route that does not exist is not a Proposal that does not exist. This
    // is the distinction a real Add lost: a 404 from an origin that never owned
    // the path read as authoritative state.
    assert(!eidolon::IsFinishedProposalProblem(404, "Not Found"));
    assert(!eidolon::IsFinishedProposalProblem(404, "{\"detail\":\"Not Found\"}"));
    assert(!eidolon::IsFinishedProposalProblem(404, ""));

    // Nor is a Decision still pending, a refusal, or another Authority's answer.
    assert(!eidolon::IsFinishedProposalProblem(
        409, problem("DECISION_REQUIRED", "admission")));
    assert(!eidolon::IsFinishedProposalProblem(
        403, problem("FORBIDDEN", "admission")));
    assert(!eidolon::IsFinishedProposalProblem(
        410, problem("PROPOSAL_EXPIRED", "device-control")));
}

void TestOperationalKeyIsPresentedInTheFormTheClaimRecords()
{
    // Admission records `p256-spki:<base64>`; Device Control compares that
    // string exactly. The device also has a bare base64 form, for the
    // pre-canonical signed-request header, and sending that one to Device
    // Control is a 403 with no other symptom — which is what a claimed BOX-3
    // did on every boot, forever.
    const std::string base64 = "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE";
    assert(eidolon::CanonicalOperationalPublicKeySpki(base64) ==
           "p256-spki:" + base64);
    // Idempotent, so a caller that already holds the canonical form is safe.
    assert(eidolon::CanonicalOperationalPublicKeySpki("p256-spki:" + base64) ==
           "p256-spki:" + base64);
    assert(eidolon::CanonicalOperationalPublicKeySpki("").empty());
}

}  // namespace

int main()
{
    TestMdnsConsumesOnlyDescriptorContract();
    TestDescriptorCanonicalisationMatchesTheGoldenVector();
    TestDescriptorWithoutItsOwnRouteIsRejected();
    TestCanonicalManifest();
    TestActiveClaimConfigurationAndProviderBinding();
    TestFinishedProposalIsRecognizedOnlyFromTheAuthoritysOwnWords();
    TestOperationalKeyIsPresentedInTheFormTheClaimRecords();
    return 0;
}
