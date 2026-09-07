#include "eidolon/device_control_proof_documents.h"

#include <cJSON.h>

#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

using eidolon::device_foundation::v1::DeviceRef;

namespace {

// The Device Foundation vectors, synced byte-for-byte from the SDK by
// scripts/sync_device_foundation_v1.py. Read rather than restated: these two
// documents are never sent, so the only way this device learns it spells one
// differently from the Authority is a refused signature — and for the
// configuration request that arrives as an active Claim with no channel, which
// nothing here or upstream distinguishes from still waiting.
std::string ReadVector(const char* name)
{
    std::ifstream file(std::string("tests/fixtures/device_foundation_v1/") + name);
    assert(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const cJSON* Required(const cJSON* object, const char* key)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    assert(item != nullptr);
    return item;
}

std::string VectorString(const cJSON* object, const char* key)
{
    const cJSON* item = Required(object, key);
    assert(cJSON_IsString(item));
    return item->valuestring;
}

uint64_t VectorNumber(const cJSON* object, const char* key)
{
    const cJSON* item = Required(object, key);
    assert(cJSON_IsNumber(item));
    assert(item->valuedouble >= 0);
    return static_cast<uint64_t>(item->valuedouble);
}

DeviceRef RefFrom(const cJSON* document)
{
    const cJSON* member = Required(document, "device_ref");
    DeviceRef ref;
    ref.device_instance_id = VectorString(member, "device_instance_id");
    ref.owner_domain_id.value = VectorString(member, "owner_domain_id");
    ref.owner_domain_generation = VectorNumber(member, "owner_domain_generation");
    ref.claim_generation = VectorNumber(member, "claim_generation");
    ref.trust_epoch = VectorNumber(member, "trust_epoch");
    return ref;
}

void TheConfigurationProofBytesAreTheGoldenVectorBytes()
{
    const std::string raw = ReadVector("device-control-configuration-proof.json");
    cJSON* vector = cJSON_ParseWithLength(raw.data(), raw.size());
    assert(cJSON_IsObject(vector));
    const cJSON* document = Required(vector, "document");
    const std::string expected = VectorString(vector, "canonical_utf8");

    assert(eidolon::device_control::ConfigurationProofJson(
               RefFrom(document), VectorString(document, "nonce")) == expected);
    cJSON_Delete(vector);
}

void TheManifestAssertionProofBytesAreTheGoldenVectorBytes()
{
    const std::string raw = ReadVector("device-control-manifest-assertion-proof.json");
    cJSON* vector = cJSON_ParseWithLength(raw.data(), raw.size());
    assert(cJSON_IsObject(vector));
    const cJSON* document = Required(vector, "document");
    const std::string expected = VectorString(vector, "canonical_utf8");

    assert(eidolon::device_control::ManifestAssertionProofJson(
               RefFrom(document), VectorString(document, "manifest_digest"),
               VectorString(document, "nonce")) == expected);
    cJSON_Delete(vector);
}

void TheTwoDocumentsAreNotTheSameDocument()
{
    // A fixture pair that had collapsed into one, or a builder that ignored the
    // member distinguishing them, would leave both assertions above passing
    // while this device signed the wrong question.
    const std::string configuration =
        ReadVector("device-control-configuration-proof.json");
    const std::string assertion =
        ReadVector("device-control-manifest-assertion-proof.json");
    assert(!configuration.empty() && !assertion.empty());
    assert(configuration != assertion);

    DeviceRef ref;
    ref.device_instance_id = "device-instance-aa";
    ref.owner_domain_id.value = "owner-domain_01";
    ref.owner_domain_generation = 1;
    ref.claim_generation = 1;
    ref.trust_epoch = 1;
    assert(eidolon::device_control::ConfigurationProofJson(ref, "nonce") !=
           eidolon::device_control::ManifestAssertionProofJson(ref, "sha256:aa", "nonce"));

    // And a member that needs escaping gets it. The nonce is base64url on every
    // real path, so nothing on device would ever have shown this up.
    assert(eidolon::device_control::ConfigurationProofJson(ref, "a\"b")
               .find("\\\"") != std::string::npos);
}

}  // namespace

int main()
{
    TheConfigurationProofBytesAreTheGoldenVectorBytes();
    TheManifestAssertionProofBytesAreTheGoldenVectorBytes();
    TheTwoDocumentsAreNotTheSameDocument();
    return 0;
}
