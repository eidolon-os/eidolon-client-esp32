#include <cassert>
#include <map>
#include <string>

#include "eidolon/hub_onboarding_protocol.h"
#include "eidolon/hub_txt_parser.h"

namespace {

using eidolon::HubConfigStatus;

eidolon::AuthorityCandidateRecord Advertised()
{
    eidolon::AuthorityCandidateRecord value;
    value.txtvers = 1;
    value.owner_domain_id = "owner_01";
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
                {"owner_domain_id", "owner_01"},
                {"owner_domain_descriptor_uri",
                 "http://eidolon-hub.local/descriptor"}},
               record) == ESP_ERR_INVALID_RESPONSE);
}

void TestDescriptorParsesToCanonicalSignedDocument()
{
    const std::string descriptor =
        "{\"owner_domain_id\":\"owner_01\",\"owner_domain_generation\":3,\"directory_revision\":7,"
        "\"trust_root_refs\":[\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"],"
        "\"endpoints\":[{\"authority\":\"admission\","
        "\"logical_audience\":\"eidolon-admission\","
        "\"uri\":\"https://host-a.owner.test/api/device-onboarding/v1\","
        "\"transport_profile\":\"https-json\",\"priority\":10}],"
        "\"issued_at\":\"2026-08-18T00:00:00Z\","
        "\"expires_at\":\"2026-08-19T00:00:00Z\","
        "\"signing_key_id\":\"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
        "\"signature\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}";
    eidolon::device_foundation::v1::OwnerDomainDescriptor parsed;
    std::string canonical;
    assert(eidolon::ParseOwnerDomainDescriptor(descriptor, parsed, canonical));
    assert(parsed.owner_domain_id == "owner_01");
    assert(parsed.owner_domain_generation == 3);
    assert(canonical ==
           "{\"directory_revision\":7,\"endpoints\":[{\"authority\":\"admission\","
           "\"logical_audience\":\"eidolon-admission\",\"priority\":10,"
           "\"transport_profile\":\"https-json\","
           "\"uri\":\"https://host-a.owner.test/api/device-onboarding/v1\"}],"
           "\"expires_at\":\"2026-08-19T00:00:00Z\","
           "\"issued_at\":\"2026-08-18T00:00:00Z\","
           "\"owner_domain_generation\":3,"
           "\"owner_domain_id\":\"owner_01\","
           "\"signing_key_id\":\"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
           "\"trust_root_refs\":[\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"]}");
    assert(parsed.endpoints.size() == 1);
    assert(parsed.endpoints.front().authority ==
           eidolon::device_foundation::v1::LogicalAuthority::Admission);
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

eidolon::HubOnboardingState PendingState()
{
    eidolon::HubOnboardingState state;
    state.owner_domain_id = "owner_01";
    state.owner_domain_generation = 3;
    state.directory_revision = 7;
    state.device_id = "aa:bb";
    state.request_id = "enroll-a";
    state.retrieval_token = "r";
    assert(state.has_local_intent());
    return state;
}

void TestReceiptHandoffAndProviderBinding()
{
    auto state = PendingState();
    const std::string receipt =
        "{\"operation\":\"device.enrollment-received\","
        "\"request_id\":\"enroll-a\",\"enrollment_id\":\"enrollment-1\","
        "\"device_id\":\"aa:bb\",\"lifecycle_state\":\"pending-approval\","
        "\"retrieval_expires_at_ms\":1786000000000}";
    eidolon::HubEnrollmentReceipt parsed;
    assert(eidolon::ParseEnrollmentReceiptResponse(receipt, state, parsed));
    state.enrollment_id = parsed.enrollment_id;

    const std::string pending =
        "{\"operation\":\"device.handoff-outcome\","
        "\"request_id\":\"handoff-a\",\"enrollment_id\":\"enrollment-1\","
        "\"device_id\":\"aa:bb\",\"manifest_revision\":\"sha256:m\","
        "\"lifecycle_state\":\"pending-approval\",\"device_ref\":null,"
        "\"channels\":[]}";
    HubConfigStatus status = HubConfigStatus::Active;
    eidolon::HubChannelAssignment assignment;
    eidolon::device_foundation::v1::DeviceRef device_ref;
    assert(eidolon::ParseHandoffResponse(pending, "handoff-a", state, status,
                                         assignment, device_ref));
    assert(status == HubConfigStatus::PendingApproval);

    const std::string approved =
        "{\"operation\":\"device.handoff-outcome\","
        "\"request_id\":\"handoff-a\",\"enrollment_id\":\"enrollment-1\","
        "\"device_id\":\"aa:bb\",\"manifest_revision\":\"sha256:m\","
        "\"lifecycle_state\":\"approved\",\"device_ref\":{"
        "\"device_instance_id\":\"aa:bb\",\"owner_domain_id\":\"owner_01\","
        "\"owner_domain_generation\":3,\"claim_generation\":1,"
        "\"trust_epoch\":1,\"accepted_manifest_digest\":"
        "\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
        "\"channels\":[{"
        "\"channel_id\":\"livekit-1\",\"purpose\":\"voice\","
        "\"kinds\":[\"audio\"],\"binding_format\":"
        "\"application/vnd.eidolon.livekit-session+json;v=2\","
        "\"issued_at_ms\":1,\"expires_at_ms\":1786000000000,"
        "\"opaque_binding\":\"e30=\"}]}";
    assert(eidolon::ParseHandoffResponse(approved, "handoff-a", state, status,
                                         assignment, device_ref));
    assert(status == HubConfigStatus::Active);
    assert(assignment.channel_id == "livekit-1");
    assert(assignment.expires_at_ms == 1786000000000);
    assert(device_ref.owner_domain_generation == 3);

    eidolon::ActiveClaimState active_claim;
    active_claim.device_ref = device_ref;
    const std::string configuration =
        "{\"operation\":\"device-control.configuration\","
        "\"nonce\":\"configuration-nonce\",\"device_ref\":{"
        "\"device_instance_id\":\"aa:bb\",\"owner_domain_id\":\"owner_01\","
        "\"owner_domain_generation\":3,\"claim_generation\":1,"
        "\"trust_epoch\":1,\"accepted_manifest_digest\":"
        "\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
        "\"lifecycle_state\":\"approved\",\"channels\":[{"
        "\"channel_id\":\"livekit-1\",\"purpose\":\"voice\","
        "\"kinds\":[\"audio\"],\"binding_format\":"
        "\"application/vnd.eidolon.livekit-session+json;v=2\","
        "\"issued_at_ms\":1,\"expires_at_ms\":1786000000000,"
        "\"opaque_binding\":\"e30=\"}]}";
    assert(eidolon::ParseDeviceConfigurationResponse(
        configuration, "configuration-nonce", active_claim, status,
        assignment));

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

}  // namespace

int main()
{
    TestMdnsConsumesOnlyDescriptorContract();
    TestDescriptorParsesToCanonicalSignedDocument();
    TestCanonicalManifest();
    TestReceiptHandoffAndProviderBinding();
    return 0;
}
