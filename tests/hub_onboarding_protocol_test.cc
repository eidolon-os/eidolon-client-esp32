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

// The canonical Manifest vectors, synced byte-for-byte from the SDK by
// scripts/sync_device_foundation_v1.py. This test used to assert against the
// bytes restated inside it, and a copy is a second authority: a field the
// contract gained, renamed, or changed the vocabulary of left this suite green
// while every device built a document the Authority would refuse — and the
// admission entry refuses one now, so that device would not enrol at all.
std::string ReadDeviceManifestVector()
{
    std::ifstream file("tests/fixtures/device_foundation_v1/device-manifest.json");
    assert(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// What this build says about turn taking, read back out of what it produced.
// It follows the compile-time profile and nothing else, so it is also what
// decides which vectors this build has to be byte-identical to.
std::string CompiledInteractionMode(const std::string& manifest)
{
    cJSON* root = cJSON_ParseWithLength(manifest.data(), manifest.size());
    assert(cJSON_IsObject(root));
    const cJSON* properties = cJSON_GetObjectItemCaseSensitive(root, "properties");
    assert(cJSON_IsArray(properties));
    std::string mode;
    const cJSON* property = nullptr;
    cJSON_ArrayForEach(property, properties) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(property, "name");
        if (!cJSON_IsString(name) || std::string(name->valuestring) != "interaction_mode") {
            continue;
        }
        const cJSON* schema = cJSON_GetObjectItemCaseSensitive(property, "schema");
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(schema, "const");
        assert(cJSON_IsString(value));
        mode = value->valuestring;
    }
    cJSON_Delete(root);
    return mode;
}

// The session binding vector, synced from the SDK. The Channel Provider writes
// this document and this device parses it, and until the vector existed each of
// them held its own hand-written copy of the shape: `schema_version: 2` is the
// record of one such disagreement already having happened, with nothing that
// would have reported it.
std::string ReadLiveKitBindingVector()
{
    std::ifstream file(
        "tests/fixtures/device_foundation_v1/livekit-session-binding.json");
    assert(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void TestLiveKitBindingMatchesTheGoldenVector()
{
    const std::string raw = ReadLiveKitBindingVector();
    cJSON* vector = cJSON_ParseWithLength(raw.data(), raw.size());
    assert(cJSON_IsObject(vector));

    // The media type is a literal on both sides and carries the document's
    // version inside it, so bumping it on one side alone is the exact change
    // that must not be quiet.
    const cJSON* format =
        cJSON_GetObjectItemCaseSensitive(vector, "binding_format");
    assert(cJSON_IsString(format));
    assert(std::string(eidolon::kLiveKitBindingFormat) == format->valuestring);

    const cJSON* canonical =
        cJSON_GetObjectItemCaseSensitive(vector, "canonical_utf8");
    assert(cJSON_IsString(canonical));
    const cJSON* binding = cJSON_GetObjectItemCaseSensitive(vector, "binding");
    assert(cJSON_IsObject(binding));
    const cJSON* session = cJSON_GetObjectItemCaseSensitive(binding, "session");
    const cJSON* audio = cJSON_GetObjectItemCaseSensitive(binding, "audio");
    assert(cJSON_IsObject(session) && cJSON_IsObject(audio));

    eidolon::Esp32HubConfig parsed;
    assert(eidolon::ParseLiveKitBinding(canonical->valuestring, parsed));
    assert(parsed.session.server_url ==
           cJSON_GetObjectItemCaseSensitive(session, "server_url")->valuestring);
    assert(parsed.session.token ==
           cJSON_GetObjectItemCaseSensitive(session, "token")->valuestring);
    assert(parsed.session.identity ==
           cJSON_GetObjectItemCaseSensitive(session, "identity")->valuestring);
    assert(parsed.session.room_name ==
           cJSON_GetObjectItemCaseSensitive(session, "room_name")->valuestring);
    assert(parsed.sample_rate ==
           cJSON_GetObjectItemCaseSensitive(audio, "sample_rate")->valueint);
    assert(parsed.channels ==
           cJSON_GetObjectItemCaseSensitive(audio, "channels")->valueint);

    // Every document the vector says must be refused, and every one it says
    // may be. This board owns its capture — it feeds PCM into the transport
    // itself — so it is on the side of `may_refuse` that does refuse, and that
    // is worth pinning: a Body whose transport negotiates the audio format
    // must not refuse those, and this one is not that Body.
    int refused = 0;
    for (const char* list : {"must_refuse", "may_refuse"}) {
        const cJSON* refusals = cJSON_GetObjectItemCaseSensitive(vector, list);
        assert(cJSON_IsArray(refusals));
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, refusals) {
            const cJSON* document =
                cJSON_GetObjectItemCaseSensitive(item, "binding");
            assert(cJSON_IsObject(document));
            char* encoded = cJSON_PrintUnformatted(document);
            assert(encoded != nullptr);
            eidolon::Esp32HubConfig rejected;
            assert(!eidolon::ParseLiveKitBinding(encoded, rejected));
            cJSON_free(encoded);
            ++refused;
        }
    }
    // Silence is not agreement: an empty list would leave the refusals
    // entirely unchecked with this test still green.
    assert(refused == 4);
    const cJSON* routing = cJSON_GetObjectItemCaseSensitive(vector, "routing");
    assert(cJSON_IsObject(routing));
    for (const char* name : {"accept", "refuse"}) {
        const cJSON* cases = cJSON_GetObjectItemCaseSensitive(routing, name);
        assert(cJSON_IsArray(cases) && cJSON_GetArraySize(cases) > 0);
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, cases) {
            cJSON* document = cJSON_Duplicate(binding, true);
            cJSON_ReplaceItemInObject(document, "session", cJSON_Duplicate(
                cJSON_GetObjectItemCaseSensitive(item, "session"), true));
            char* encoded = cJSON_PrintUnformatted(document);
            eidolon::Esp32HubConfig candidate;
            assert(eidolon::ParseLiveKitBinding(encoded, candidate) == (std::string(name) == "accept"));
            cJSON_free(encoded);
            cJSON_Delete(document);
        }
    }

    cJSON_Delete(vector);
}

void TestCanonicalManifest()
{
    const std::string raw = ReadDeviceManifestVector();
    cJSON* vector = cJSON_ParseWithLength(raw.data(), raw.size());
    assert(cJSON_IsObject(vector));
    const cJSON* cases = cJSON_GetObjectItemCaseSensitive(vector, "cases");
    assert(cJSON_IsArray(cases));

    // A device states its turn-taking as an immutable property, so the Provider
    // reads a fact rather than guessing one. Half duplex here, pinned by
    // tests/stubs/sdkconfig.h.
    const std::string mode =
        CompiledInteractionMode(eidolon::BuildDeviceManifestJson("probe", /*has_camera=*/false));
    assert(mode == "half_duplex");

    int matched = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cases) {
        const cJSON* declared = cJSON_GetObjectItemCaseSensitive(item, "interaction_mode");
        assert(cJSON_IsString(declared));
        if (mode != declared->valuestring) {
            continue;
        }
        const cJSON* board = cJSON_GetObjectItemCaseSensitive(item, "board_name");
        const cJSON* camera = cJSON_GetObjectItemCaseSensitive(item, "has_camera");
        const cJSON* bytes = cJSON_GetObjectItemCaseSensitive(item, "canonical_utf8");
        assert(cJSON_IsString(board) && cJSON_IsBool(camera) && cJSON_IsString(bytes));
        assert(eidolon::BuildDeviceManifestJson(board->valuestring, cJSON_IsTrue(camera)) ==
               bytes->valuestring);
        ++matched;
    }
    // Silence is not agreement. A vector file this build matches nothing in
    // would leave the producer entirely unchecked with this test still green,
    // which is the failure mode the inlined copy had.
    assert(matched == 2);
    cJSON_Delete(vector);
}

// The configuration response vector. Distinct from the inline bodies in
// TestActiveClaimConfigurationAndProviderBinding below, and both should stay:
// those probe what this parser tolerates (a manifest it has never seen, the
// retired two-room shape), while this one pins what the Authority actually
// emits and what a Body must conclude from it. The conclusion is the part worth
// pinning — `lifecycle_state` and the presence of a channel are separate facts,
// and reading approved-with-no-channel as failure abandons an enrolment that is
// fine.
std::string ReadConfigurationResponseVector()
{
    std::ifstream file(
        "tests/fixtures/device_foundation_v1/device-control-configuration-response.json");
    assert(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

eidolon::ActiveClaimState ClaimFrom(const cJSON* device_ref)
{
    eidolon::ActiveClaimState claim;
    claim.device_ref.device_instance_id =
        cJSON_GetObjectItemCaseSensitive(device_ref, "device_instance_id")->valuestring;
    claim.device_ref.owner_domain_id.value =
        cJSON_GetObjectItemCaseSensitive(device_ref, "owner_domain_id")->valuestring;
    claim.device_ref.owner_domain_generation = static_cast<uint64_t>(
        cJSON_GetObjectItemCaseSensitive(device_ref, "owner_domain_generation")->valuedouble);
    claim.device_ref.claim_generation = static_cast<uint64_t>(
        cJSON_GetObjectItemCaseSensitive(device_ref, "claim_generation")->valuedouble);
    claim.device_ref.trust_epoch = static_cast<uint64_t>(
        cJSON_GetObjectItemCaseSensitive(device_ref, "trust_epoch")->valuedouble);
    return claim;
}

void TestConfigurationResponseMatchesTheGoldenVector()
{
    const std::string raw = ReadConfigurationResponseVector();
    cJSON* vector = cJSON_ParseWithLength(raw.data(), raw.size());
    assert(cJSON_IsObject(vector));
    const std::string nonce =
        cJSON_GetObjectItemCaseSensitive(vector, "request_nonce")->valuestring;
    const eidolon::ActiveClaimState claim =
        ClaimFrom(cJSON_GetObjectItemCaseSensitive(vector, "device_ref"));

    int accepted = 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(vector, "cases")) {
        const std::string body_state =
            cJSON_GetObjectItemCaseSensitive(item, "body_state")->valuestring;
        const std::string body =
            cJSON_GetObjectItemCaseSensitive(item, "canonical_utf8")->valuestring;
        HubConfigStatus status = HubConfigStatus::PendingApproval;
        eidolon::HubChannelAssignment assignment;
        eidolon::AcceptedManifestRef accepted_manifest;
        assert(eidolon::ParseDeviceConfigurationResponse(
            body, nonce, claim, status, assignment, accepted_manifest));
        if (body_state == "active") {
            assert(status == HubConfigStatus::Active);
            assert(!assignment.opaque_binding.empty());
        } else if (body_state == "awaiting-channel") {
            // The one that matters. Not revoked, not an error: the Authority
            // holds the Claim and the Channel has not answered yet.
            assert(status == HubConfigStatus::WaitingBinding);
            assert(assignment.opaque_binding.empty());
        } else {
            assert(body_state == "revoked");
            assert(status == HubConfigStatus::Revoked);
        }
        ++accepted;
    }
    // Silence is not agreement: a vector that lost the awaiting-channel case
    // would leave the distinction this test exists for entirely unchecked.
    assert(accepted == 3);

    int refused = 0;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(vector, "must_refuse")) {
        const cJSON* document = cJSON_GetObjectItemCaseSensitive(item, "response");
        char* encoded = cJSON_PrintUnformatted(document);
        assert(encoded != nullptr);
        HubConfigStatus status = HubConfigStatus::PendingApproval;
        eidolon::HubChannelAssignment assignment;
        eidolon::AcceptedManifestRef accepted_manifest;
        assert(!eidolon::ParseDeviceConfigurationResponse(
            encoded, nonce, claim, status, assignment, accepted_manifest));
        cJSON_free(encoded);
        ++refused;
    }
    assert(refused == 4);
    cJSON_Delete(vector);
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


    std::string candidates = binding;
    const auto start = candidates.find("\"server_url\":");
    candidates.insert(start, "\"server_urls\":[\"wss://lk\",\"wss://other\"],");
    assert(eidolon::ParseLiveKitBinding(candidates, config));
    assert(config.session.server_urls.size() == 2);
    candidates.replace(candidates.find("wss://other"), 11, "wss://lk");
    assert(!eidolon::ParseLiveKitBinding(candidates, config));
    assert(eidolon::ParseLiveKitBinding(binding, config));
    assert(config.session.server_urls.empty());

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
    TestLiveKitBindingMatchesTheGoldenVector();
    TestConfigurationResponseMatchesTheGoldenVector();
    TestActiveClaimConfigurationAndProviderBinding();
    TestFinishedProposalIsRecognizedOnlyFromTheAuthoritysOwnWords();
    TestOperationalKeyIsPresentedInTheFormTheClaimRecords();
    return 0;
}
