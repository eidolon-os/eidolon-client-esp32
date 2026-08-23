#include "device_identity.h"

#include "settings.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <mbedtls/asn1.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/esp_mbedtls_random.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <ctime>
#include <cstring>

#define TAG "DeviceIdentity"

namespace eidolon {

namespace {

constexpr const char* kSettingsNs = "eidolon_id";
constexpr const char* kPrivateKeyKey = "p256_priv";

std::string Hex(const unsigned char* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

std::string Base64Url(const unsigned char* data, size_t len) {
    size_t needed = 0;
    mbedtls_base64_encode(nullptr, 0, &needed, data, len);
    std::string out;
    out.resize(needed);
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &written,
                              data, len) != 0) {
        return "";
    }
    out.resize(written);
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    for (char& ch : out) {
        if (ch == '+') {
            ch = '-';
        } else if (ch == '/') {
            ch = '_';
        }
    }
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

std::string NewNonce() {
    unsigned char bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    return Base64Url(bytes, sizeof(bytes));
}

std::string Sha256Hex(const std::string& body) {
    unsigned char hash[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(body.data()), body.size(), hash, 0);
    return Hex(hash, sizeof(hash));
}

std::string RequestTimestamp() {
    time_t now = time(nullptr);
    if (now > 0) {
        return std::to_string(static_cast<long long>(now));
    }
    return std::to_string(static_cast<long long>(esp_timer_get_time() / 1000000));
}

esp_err_t SeedCtrDrbg(mbedtls_entropy_context& entropy, mbedtls_ctr_drbg_context& ctr_drbg) {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    const char* personal = "eidolon-device";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char*>(personal),
                                    strlen(personal));
    if (ret != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed: -0x%04x", -ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool DerEcdsaToRaw(const unsigned char* der, size_t der_len,
                   unsigned char raw[64]) {
    unsigned char* cursor = const_cast<unsigned char*>(der);
    const unsigned char* end = der + der_len;
    size_t sequence_len = 0;
    if (mbedtls_asn1_get_tag(&cursor, end, &sequence_len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0 ||
        cursor + sequence_len != end) {
        return false;
    }
    for (size_t index = 0; index < 2; ++index) {
        size_t integer_len = 0;
        if (mbedtls_asn1_get_tag(&cursor, end, &integer_len,
                                 MBEDTLS_ASN1_INTEGER) != 0 ||
            integer_len == 0 || cursor + integer_len > end) {
            return false;
        }
        while (integer_len > 32 && *cursor == 0) {
            ++cursor;
            --integer_len;
        }
        if (integer_len > 32) return false;
        std::memset(raw + index * 32, 0, 32);
        std::memcpy(raw + index * 32 + 32 - integer_len, cursor, integer_len);
        cursor += integer_len;
    }
    return cursor == end;
}

}  // namespace

DeviceIdentity& DeviceIdentity::GetInstance() {
    static DeviceIdentity instance;
    return instance;
}

esp_err_t DeviceIdentity::EnsureKeypair() {
    if (!private_key_pem_.empty() && !public_key_b64_.empty()) {
        return ESP_OK;
    }
    return LoadOrCreateKey();
}

esp_err_t DeviceIdentity::LoadOrCreateKey() {
    Settings settings(kSettingsNs, true);
    private_key_pem_ = settings.GetString(kPrivateKeyKey);
    if (private_key_pem_.empty()) {
        return CreateKeypair();
    }
    return LoadPublicKeyFromPrivateKey();
}

esp_err_t DeviceIdentity::CreateKeypair() {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    if (SeedCtrDrbg(entropy, ctr_drbg) != ESP_OK) {
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return ESP_FAIL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret == 0) {
        ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                                  mbedtls_ctr_drbg_random, &ctr_drbg);
    }
    if (ret != 0) {
        ESP_LOGE(TAG, "P-256 key generation failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return ESP_FAIL;
    }

    unsigned char pem[512] = {};
    ret = mbedtls_pk_write_key_pem(&pk, pem, sizeof(pem));
    if (ret != 0) {
        ESP_LOGE(TAG, "write private key PEM failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return ESP_FAIL;
    }
    private_key_pem_ = reinterpret_cast<const char*>(pem);
    Settings settings(kSettingsNs, true);
    settings.SetString(kPrivateKeyKey, private_key_pem_);

    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    ESP_LOGI(TAG, "Generated device P-256 keypair");
    return LoadPublicKeyFromPrivateKey();
}

esp_err_t DeviceIdentity::LoadPublicKeyFromPrivateKey() {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_key(&pk,
                                   reinterpret_cast<const unsigned char*>(private_key_pem_.c_str()),
                                   private_key_pem_.size() + 1, nullptr, 0, mbedtls_esp_random,
                                   nullptr);
    if (ret != 0) {
        ESP_LOGE(TAG, "parse private key failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }

    unsigned char der[256] = {};
    ret = mbedtls_pk_write_pubkey_der(&pk, der, sizeof(der));
    if (ret < 0) {
        ESP_LOGE(TAG, "write public key DER failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }
    const unsigned char* public_der = der + sizeof(der) - ret;
    public_key_b64_ = Base64Url(public_der, ret);

    unsigned char hash[32];
    mbedtls_sha256(public_der, ret, hash, 0);
    fingerprint_ = "p256:" + Hex(hash, sizeof(hash));
    mbedtls_pk_free(&pk);
    return public_key_b64_.empty() ? ESP_FAIL : ESP_OK;
}

esp_err_t DeviceIdentity::SignCanonical(const std::string& canonical, std::string& signature) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    if (SeedCtrDrbg(entropy, ctr_drbg) != ESP_OK) {
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return ESP_FAIL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_key(&pk,
                                   reinterpret_cast<const unsigned char*>(private_key_pem_.c_str()),
                                   private_key_pem_.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random,
                                   &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "parse signing key failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return ESP_FAIL;
    }

    unsigned char hash[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(), hash, 0);
    unsigned char sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0;
    ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig), &sig_len,
                          mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "sign config request failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return ESP_FAIL;
    }

    unsigned char raw_signature[64];
    if (!DerEcdsaToRaw(sig, sig_len, raw_signature)) {
        ESP_LOGE(TAG, "ECDSA signature was not canonical DER");
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return ESP_FAIL;
    }
    signature = Base64Url(raw_signature, sizeof(raw_signature));
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return signature.empty() ? ESP_FAIL : ESP_OK;
}

esp_err_t DeviceIdentity::SignRequest(const std::string& method, const std::string& path_query,
                                      const std::string& device_id, const std::string& body,
                                      SignedRequestHeaders& out) {
    esp_err_t err = EnsureKeypair();
    if (err != ESP_OK) {
        return err;
    }
    out.nonce = NewNonce();
    out.timestamp = RequestTimestamp();
    out.public_key = public_key_b64_;

    std::string canonical = method + "\n" + path_query + "\n" + device_id + "\n" + out.nonce + "\n" +
                            out.timestamp + "\n" + Sha256Hex(body);
    return SignCanonical(canonical, out.signature);
}

esp_err_t DeviceIdentity::SignGetRequest(const std::string& path_query, const std::string& device_id,
                                         SignedRequestHeaders& out) {
    return SignRequest("GET", path_query, device_id, "", out);
}

std::string EidolonSignedGetPathQuery(const std::string& url) {
    size_t start = 0;
    const size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        const size_t slash = url.find('/', scheme + 3);
        if (slash == std::string::npos) {
            return "/";
        }
        start = slash;
    }
    std::string path_query = url.substr(start);
    return path_query.empty() ? "/" : path_query;
}

}  // namespace eidolon
