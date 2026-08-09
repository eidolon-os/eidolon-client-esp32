#include <cassert>
#include <map>
#include <string>

#include "eidolon/hub_onboarding_protocol.h"
#include "eidolon/hub_txt_parser.h"

namespace {

using eidolon::HubConfigStatus;

eidolon::HubTxtRecord Advertised()
{
    eidolon::HubTxtRecord value;
    value.txtvers = 1;
    value.descriptor_uri =
        "https://eidolon-hub.local/api/device-onboarding/v1/descriptor";
    value.enrollment_uri =
        "https://eidolon-hub.local/api/device-onboarding/v1/enrollments";
    return value;
}

void TestMdnsConsumesOnlyDescriptorContract()
{
    eidolon::HubTxtRecord record;
    assert(eidolon::HubTxtParser::Parse(
               {{"txtvers", "1"},
                {"descriptor_uri", Advertised().descriptor_uri},
                {"enrollment_uri", Advertised().enrollment_uri}},
               record) == ESP_OK);
    assert(record.descriptor_uri == Advertised().descriptor_uri);
    assert(eidolon::HubTxtParser::Parse(
               {{"txtvers", "1"},
                {"api", "v1"},
                {"register_url", "https://eidolon-hub.local/api/device/register"}},
               record) == ESP_ERR_INVALID_RESPONSE);
    assert(eidolon::HubTxtParser::Parse(
               {{"txtvers", "1"},
                {"descriptor_uri", "http://eidolon-hub.local/descriptor"},
                {"enrollment_uri", Advertised().enrollment_uri}},
               record) == ESP_ERR_INVALID_RESPONSE);
}

void TestDescriptorIsPinnedToAdvertisedHttpsOrigin()
{
    const auto advertised = Advertised();
    const std::string descriptor =
        "{\"schema_version\":1,\"hub_id\":\"hub-local\","
        "\"descriptor_uri\":\"" + advertised.descriptor_uri + "\","
        "\"device_onboarding_uri\":"
        "\"https://eidolon-hub.local/api/device-onboarding/v1\","
        "\"enrollment_uri\":\"" + advertised.enrollment_uri + "\","
        "\"protocol_versions\":[1]}";
    eidolon::HubDescriptor parsed;
    assert(eidolon::ParseHubDescriptorResponse(descriptor, advertised, parsed));
    assert(parsed.hub_id == "hub-local");

    std::string diverted = descriptor;
    const auto offset = diverted.find(advertised.enrollment_uri);
    diverted.replace(offset, advertised.enrollment_uri.size(),
                     "https://attacker.example/enrollments");
    assert(!eidolon::ParseHubDescriptorResponse(diverted, advertised, parsed));
}

void TestCanonicalManifest()
{
    const std::string manifest =
        eidolon::BuildDeviceManifestJson("esp32-s3-touch-amoled-2.06");
    assert(manifest ==
           "{\"actions\":[],\"events\":[],\"media\":[{\"codecs\":[\"opus\"],"
           "\"direction\":\"bidirectional\",\"kind\":\"audio\"}],\"properties\":[],"
           "\"schema_version\":1,\"title\":\"esp32-s3-touch-amoled-2.06\"}");
}

eidolon::HubOnboardingState PendingState()
{
    eidolon::HubOnboardingState state;
    state.hub_id = "hub-local";
    state.descriptor_uri = Advertised().descriptor_uri;
    state.enrollment_uri = Advertised().enrollment_uri;
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
    state.retrieval_expires_at_ms = parsed.retrieval_expires_at_ms;

    const std::string pending =
        "{\"operation\":\"device.handoff-outcome\","
        "\"request_id\":\"handoff-a\",\"enrollment_id\":\"enrollment-1\","
        "\"device_id\":\"aa:bb\",\"manifest_revision\":\"sha256:m\","
        "\"lifecycle_state\":\"pending-approval\",\"channels\":[]}";
    HubConfigStatus status = HubConfigStatus::Active;
    eidolon::HubChannelAssignment assignment;
    assert(eidolon::ParseHandoffResponse(pending, "handoff-a", state, status,
                                         assignment));
    assert(status == HubConfigStatus::PendingApproval);

    const std::string approved =
        "{\"operation\":\"device.handoff-outcome\","
        "\"request_id\":\"handoff-a\",\"enrollment_id\":\"enrollment-1\","
        "\"device_id\":\"aa:bb\",\"manifest_revision\":\"sha256:m\","
        "\"lifecycle_state\":\"approved\",\"channels\":[{"
        "\"channel_id\":\"livekit-1\",\"purpose\":\"voice\","
        "\"kinds\":[\"audio\"],\"binding_format\":"
        "\"application/vnd.eidolon.livekit-device+json;v=1\","
        "\"issued_at_ms\":1,\"expires_at_ms\":1786000000000,"
        "\"opaque_binding\":\"e30=\"}]}";
    assert(eidolon::ParseHandoffResponse(approved, "handoff-a", state, status,
                                         assignment));
    assert(status == HubConfigStatus::Active);
    assert(assignment.channel_id == "livekit-1");

    const std::string binding =
        "{\"schema_version\":1,\"active\":{\"server_url\":\"wss://lk\","
        "\"token\":\"voice\",\"identity\":\"device\",\"room_name\":\"voice\"},"
        "\"control\":{\"server_url\":\"wss://lk\",\"token\":\"control\","
        "\"identity\":\"device\",\"room_name\":\"control\"},"
        "\"audio\":{\"sample_rate\":16000,\"channels\":1}}";
    eidolon::Esp32HubConfig config;
    assert(eidolon::ParseLiveKitBinding(binding, config));
    assert(config.active.room_name == "voice");
    assert(config.control.room_name == "control");

}

}  // namespace

int main()
{
    TestMdnsConsumesOnlyDescriptorContract();
    TestDescriptorIsPinnedToAdvertisedHttpsOrigin();
    TestCanonicalManifest();
    TestReceiptHandoffAndProviderBinding();
    return 0;
}
