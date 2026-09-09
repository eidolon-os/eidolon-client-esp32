#include "esp_idf_claim_grant_crypto.h"

#include "claim_grant_hpke_schedule.h"
#include "device_identity.h"
#include "settings.h"

#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_random.h>
#include "mbedtls_compat.h"
#include <mbedtls/asn1.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>

#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace eidolon {
namespace {

constexpr char kNamespace[] = "eidolon_claim";
constexpr char kHandoffPrivateKey[] = "handoff_priv";

std::string Hex(const unsigned char* bytes, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '\0');
    for (size_t index = 0; index < size; ++index) {
        result[index * 2] = digits[bytes[index] >> 4];
        result[index * 2 + 1] = digits[bytes[index] & 0x0f];
    }
    return result;
}

std::string Base64Url(const unsigned char* bytes, size_t size) {
    size_t capacity = 4 * ((size + 2) / 3) + 1;
    std::string result(capacity, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char*>(result.data()), result.size(),
            &written, bytes, size) != 0) return {};
    result.resize(written);
    for (char& value : result) {
        if (value == '+') value = '-';
        else if (value == '/') value = '_';
    }
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

bool Base64UrlDecode(const std::string& encoded,
                     std::vector<unsigned char>& out) {
    std::string standard = encoded;
    for (char& value : standard) {
        if (value == '-') value = '+';
        else if (value == '_') value = '/';
    }
    standard.append((4 - standard.size() % 4) % 4, '=');
    size_t required = 0;
    const int sized = mbedtls_base64_decode(
        nullptr, 0, &required,
        reinterpret_cast<const unsigned char*>(standard.data()),
        standard.size());
    if (sized != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required == 0 ||
        required > 64 * 1024) return false;
    out.resize(required);
    size_t written = 0;
    if (mbedtls_base64_decode(
            out.data(), out.size(), &written,
            reinterpret_cast<const unsigned char*>(standard.data()),
            standard.size()) != 0) return false;
    out.resize(written);
    return true;
}

bool HmacSha256(const std::vector<unsigned char>& key,
                const std::vector<unsigned char>& input,
                std::array<unsigned char, 32>& output) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md != nullptr &&
        mbedtls_md_hmac(md, key.data(), key.size(), input.data(), input.size(),
                        output.data()) == 0;
}

std::vector<unsigned char> Bytes(const std::string& value) {
    return {value.begin(), value.end()};
}

bool Seed(mbedtls_entropy_context& entropy,
          mbedtls_ctr_drbg_context& random) {
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
    mbedtls_entropy_init(&entropy);
#else
    // See device_identity.cc: mbedtls 4 has no standalone entropy module, so the
    // DRBG is seeded from the hardware RNG this file already uses elsewhere.
    (void)entropy;
#endif
    mbedtls_ctr_drbg_init(&random);
    constexpr char personal[] = "eidolon-claim-grant";
    return mbedtls_ctr_drbg_seed(
               &random,
#if EIDOLON_MBEDTLS_LEGACY_PUBLIC
               mbedtls_entropy_func, &entropy,
#else
               eidolon_mbedtls_random, nullptr,
#endif
               reinterpret_cast<const unsigned char*>(personal),
               sizeof(personal) - 1) == 0;
}

bool DerSignatureToRaw(const unsigned char* der, size_t der_size,
                       unsigned char raw[64]) {
    unsigned char* cursor = const_cast<unsigned char*>(der);
    const unsigned char* end = der + der_size;
    size_t sequence_size = 0;
    if (mbedtls_asn1_get_tag(
            &cursor, end, &sequence_size,
            MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0 ||
        cursor + sequence_size != end) return false;
    for (size_t part = 0; part < 2; ++part) {
        size_t integer_size = 0;
        if (mbedtls_asn1_get_tag(
                &cursor, end, &integer_size, MBEDTLS_ASN1_INTEGER) != 0 ||
            integer_size == 0 || cursor + integer_size > end) return false;
        while (integer_size > 32 && *cursor == 0) {
            ++cursor;
            --integer_size;
        }
        if (integer_size > 32) return false;
        std::memset(raw + part * 32, 0, 32);
        std::memcpy(raw + part * 32 + 32 - integer_size, cursor, integer_size);
        cursor += integer_size;
    }
    return cursor == end;
}

bool PublicKeyFacts(mbedtls_pk_context& key, std::string& public_key,
                    std::string& key_id) {
    unsigned char der[256] = {};
    const int size = mbedtls_pk_write_pubkey_der(&key, der, sizeof(der));
    if (size <= 0) return false;
    const unsigned char* encoded = der + sizeof(der) - size;
    unsigned char digest[32] = {};
    if (mbedtls_sha256(encoded, size, digest, 0) != 0) return false;
    public_key = "p256-spki:" + Base64Url(encoded, size);
    key_id = "sha256:" + Hex(digest, sizeof(digest));
    return public_key.size() > 20 && key_id.size() == 71;
}

bool SignPem(const std::string& pem, const std::string& canonical,
             std::string& signature) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    if (!Seed(entropy, random)) return false;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int result = eidolon_pk_parse_key(
        &key, reinterpret_cast<const unsigned char*>(pem.c_str()),
        pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &random);
    unsigned char digest[32] = {};
    unsigned char der[MBEDTLS_PK_SIGNATURE_MAX_SIZE] = {};
    size_t der_size = 0;
    if (result == 0) {
        result = mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(canonical.data()),
            canonical.size(), digest, 0);
    }
    if (result == 0) {
        result = eidolon_pk_sign(
            &key, MBEDTLS_MD_SHA256, digest, sizeof(digest), der, sizeof(der),
            &der_size, mbedtls_ctr_drbg_random, &random);
    }
    unsigned char raw[64] = {};
    const bool valid = result == 0 && DerSignatureToRaw(der, der_size, raw);
    if (valid) signature = Base64Url(raw, sizeof(raw));
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&random);
    EIDOLON_ENTROPY_FREE(&entropy);
    return valid && signature.size() == 86;
}

std::string Text(const cJSON* object, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool Uint(const cJSON* object, const char* key, uint64_t& output) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 1 ||
        value->valuedouble > 9007199254740991.0) return false;
    output = static_cast<uint64_t>(value->valuedouble);
    return static_cast<double>(output) == value->valuedouble;
}

bool ParseClaimGrant(const std::string& json,
                     device_foundation::v1::ClaimGrant& grant) {
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    const cJSON* ref = cJSON_GetObjectItemCaseSensitive(root, "device_ref");
    const cJSON* manifest = cJSON_GetObjectItemCaseSensitive(root, "manifest_ref");
    uint64_t claim_generation = 0;
    uint64_t trust_epoch = 0;
    bool valid = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 9 &&
        cJSON_IsObject(ref) && cJSON_GetArraySize(ref) == 5 &&
        cJSON_IsObject(manifest) && cJSON_GetArraySize(manifest) == 3;
    grant = {};
    if (valid) {
        grant.grant_id = Text(root, "grant_id");
        grant.enrollment_id = Text(root, "enrollment_id");
        grant.approval_decision_id = Text(root, "approval_decision_id");
        grant.handoff_key_id = Text(root, "handoff_key_id");
        grant.operational_key_id = Text(root, "operational_key_id");
        grant.issued_at = Text(root, "issued_at");
        grant.expires_at = Text(root, "expires_at");
        grant.device_ref.device_instance_id = Text(ref, "device_instance_id");
        grant.device_ref.owner_domain_id.value = Text(ref, "owner_domain_id");
        grant.manifest_ref.manifest_id = Text(manifest, "manifest_id");
        grant.manifest_ref.digest = Text(manifest, "digest");
        valid = Uint(ref, "owner_domain_generation",
                     grant.device_ref.owner_domain_generation) &&
            Uint(ref, "claim_generation", claim_generation) &&
            Uint(ref, "trust_epoch", trust_epoch) &&
            Uint(manifest, "revision", grant.manifest_ref.revision) &&
            claim_generation <= std::numeric_limits<uint32_t>::max() &&
            trust_epoch <= std::numeric_limits<uint32_t>::max();
        if (valid) {
            grant.device_ref.claim_generation =
                static_cast<uint32_t>(claim_generation);
            grant.device_ref.trust_epoch = static_cast<uint32_t>(trust_epoch);
        }
    }
    cJSON_Delete(root);
    return valid;
}

}  // namespace

bool EspIdfClaimGrantCrypto::LoadHandoffPrivateKey(std::string& pem) const {
    Settings settings(kNamespace, false);
    pem = settings.GetString(kHandoffPrivateKey);
    return !pem.empty();
}

bool EspIdfClaimGrantCrypto::EnsureEnrollmentMaterial() {
    auto& identity = DeviceIdentity::GetInstance();
    if (identity.EnsureKeypair() != ESP_OK) return false;
    std::string pem;
    if (!LoadHandoffPrivateKey(pem)) {
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context random;
        if (!Seed(entropy, random)) return false;
        mbedtls_pk_context key;
        mbedtls_pk_init(&key);
        int result = eidolon_pk_gen_p256(&key, &random);
        unsigned char encoded[512] = {};
        if (result == 0) result = mbedtls_pk_write_key_pem(&key, encoded, sizeof(encoded));
        if (result == 0) pem = reinterpret_cast<const char*>(encoded);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&random);
        EIDOLON_ENTROPY_FREE(&entropy);
        if (result != 0 || pem.empty()) return false;
        Settings settings(kNamespace, true);
        if (settings.SetString(kHandoffPrivateKey, pem) != ESP_OK ||
            settings.Commit() != ESP_OK) return false;
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    const int parsed = eidolon_pk_parse_key(
        &key, reinterpret_cast<const unsigned char*>(pem.c_str()),
        pem.size() + 1, nullptr, 0, eidolon_mbedtls_random, nullptr);
    const bool valid = parsed == 0 &&
        PublicKeyFacts(key, handoff_public_key_, handoff_key_id_);
    mbedtls_pk_free(&key);
    return valid;
}

std::string EspIdfClaimGrantCrypto::HandoffPublicKey() const {
    return handoff_public_key_;
}

std::string EspIdfClaimGrantCrypto::OperationalPublicKey() const {
    return DeviceIdentity::GetInstance().AdmissionOperationalPublicKey();
}

std::string EspIdfClaimGrantCrypto::HandoffKeyId() const {
    return handoff_key_id_;
}

std::string EspIdfClaimGrantCrypto::OperationalKeyId() const {
    const std::string& fingerprint = DeviceIdentity::GetInstance().Fingerprint();
    return fingerprint.rfind("p256:", 0) == 0
        ? "sha256:" + fingerprint.substr(5)
        : std::string{};
}

bool EspIdfClaimGrantCrypto::SignWithHandoff(
    const std::string& canonical, std::string& signature) const {
    std::string pem;
    return LoadHandoffPrivateKey(pem) && SignPem(pem, canonical, signature);
}

bool EspIdfClaimGrantCrypto::BuildHandoffKeyProof(
    const std::string& enrollment_id, uint64_t proposal_revision,
    const std::string& collection_challenge, std::string& proof) {
    return SignWithHandoff(
        DeviceClaimConsumerCore::ClaimGrantCollectionProofJson(
            enrollment_id, proposal_revision, collection_challenge),
        proof);
}

ClaimGrantUnsealResult EspIdfClaimGrantCrypto::OpenClaimGrant(
    const device_foundation::v1::ClaimGrantWireEnvelope& envelope,
    const std::string& canonical_aad,
    device_foundation::v1::ClaimGrant& plaintext) {
    std::string pem;
    std::vector<unsigned char> enc;
    std::vector<unsigned char> ciphertext;
    if (!LoadHandoffPrivateKey(pem) ||
        envelope.recipient_handoff_key_id != HandoffKeyId() ||
        !Base64UrlDecode(envelope.encapsulated_key, enc) || enc.size() != 65 ||
        !Base64UrlDecode(envelope.ciphertext, ciphertext) ||
        ciphertext.size() <= 16) {
        return ClaimGrantUnsealResult::AuthenticationRejected;
    }

    mbedtls_pk_context recipient;
    mbedtls_pk_init(&recipient);
    if (eidolon_pk_parse_key(
            &recipient, reinterpret_cast<const unsigned char*>(pem.c_str()),
            pem.size() + 1, nullptr, 0, eidolon_mbedtls_random, nullptr) != 0) {
        mbedtls_pk_free(&recipient);
        return ClaimGrantUnsealResult::WireAuthUnavailable;
    }
    std::vector<unsigned char> shared(32, 0);
    unsigned char recipient_point_buffer[65] = {};
    size_t recipient_point_size = 0;
    int result = eidolon_ecdh_p256(&recipient, enc.data(), enc.size(),
                                   shared.data(), recipient_point_buffer,
                                   &recipient_point_size);
    mbedtls_pk_free(&recipient);
    if (result != 0 || recipient_point_size != 65) {
        return ClaimGrantUnsealResult::AuthenticationRejected;
    }

    const std::vector<unsigned char> recipient_point(
        recipient_point_buffer,
        recipient_point_buffer + recipient_point_size);
    P256HpkeBaseContext context;
    if (!DeriveP256HpkeBaseContext(
            shared, enc, recipient_point, "eidolon-trust-p256-hpke-v1",
            HmacSha256, context)) {
        return ClaimGrantUnsealResult::WireAuthUnavailable;
    }

    const size_t plaintext_size = ciphertext.size() - 16;
    std::string decoded(plaintext_size, '\0');
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    result = mbedtls_gcm_setkey(
        &gcm, MBEDTLS_CIPHER_ID_AES, context.key.data(), 128);
    if (result == 0) {
        result = mbedtls_gcm_auth_decrypt(
            &gcm, plaintext_size, context.base_nonce.data(),
            context.base_nonce.size(),
            reinterpret_cast<const unsigned char*>(canonical_aad.data()),
            canonical_aad.size(), ciphertext.data() + plaintext_size, 16,
            ciphertext.data(), reinterpret_cast<unsigned char*>(decoded.data()));
    }
    mbedtls_gcm_free(&gcm);
    if (result != 0 || !ParseClaimGrant(decoded, plaintext)) {
        plaintext = {};
        return ClaimGrantUnsealResult::AuthenticationRejected;
    }
    return ClaimGrantUnsealResult::Authenticated;
}

bool EspIdfClaimGrantCrypto::BuildOperationalKeyProof(
    const std::string& enrollment_id, const std::string& grant_id,
    const device_foundation::v1::DeviceRef& device_ref, std::string& proof) {
    return DeviceIdentity::GetInstance().SignCanonical(
               DeviceClaimConsumerCore::ClaimGrantAckProofJson(
                   enrollment_id, grant_id, device_ref),
               proof) == ESP_OK;
}

bool EspIdfClaimGrantCrypto::DestroyEnrollmentMaterial(
    const std::string& enrollment_id, const std::string& handoff_key_id) {
    (void)enrollment_id;
    if (handoff_key_id.empty() || handoff_key_id != HandoffKeyId()) return false;
    Settings settings(kNamespace, true);
    settings.EraseKey(kHandoffPrivateKey);
    if (settings.Commit() != ESP_OK) return false;
    handoff_public_key_.clear();
    handoff_key_id_.clear();
    return true;
}

}  // namespace eidolon
