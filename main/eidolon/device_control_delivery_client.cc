#include "device_control_delivery_client.h"

#include "authority_locator.h"
#include "device_delivery_consumer_core.h"
#include "device_erase_identity_adapter.h"
#include "device_identity.h"
#include "esp_idf_device_local_erase_adapter.h"
#include "esp_idf_owner_data_erase_storage.h"
#include "hub_pinned_http.h"
#include "rfc3339_utc.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <sys/time.h>

namespace eidolon {
namespace {

constexpr const char* TAG = "DeviceControlDelivery";

std::string Quote(const std::string& value) {
    cJSON* item = cJSON_CreateString(value.c_str());
    char* printed = item ? cJSON_PrintUnformatted(item) : nullptr;
    const std::string out = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(item);
    return out;
}

std::string DeviceRefJson(const device_foundation::v1::DeviceRef& ref) {
    return std::string("{\"claim_generation\":") +
           std::to_string(ref.claim_generation) +
           ",\"device_instance_id\":" + Quote(ref.device_instance_id) +
           ",\"owner_domain_generation\":" +
           std::to_string(ref.owner_domain_generation) +
           ",\"owner_domain_id\":" + Quote(ref.owner_domain_id.value) +
           ",\"trust_epoch\":" + std::to_string(ref.trust_epoch) + "}";
}

std::string RandomNonce() {
    std::string random(18, '\0');
    esp_fill_random(random.data(), random.size());
    size_t capacity = 4 * ((random.size() + 2) / 3) + 1;
    std::string encoded(capacity, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
            &written, reinterpret_cast<const unsigned char*>(random.data()),
            random.size()) != 0) {
        return "";
    }
    encoded.resize(written);
    for (char& ch : encoded) {
        if (ch == '+') ch = '-';
        if (ch == '/') ch = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

class OperationalClock final : public DeviceEraseClockPort {
public:
    Rfc3339DeadlineState DeadlineState(
        const std::string& deadline) const override {
        timeval current{};
        if (gettimeofday(&current, nullptr) != 0) {
            return Rfc3339DeadlineState::Unknown;
        }
        const int64_t now_millis =
            static_cast<int64_t>(current.tv_sec) * 1000 +
            current.tv_usec / 1000;
        // An untrusted clock must not authorize a destructive command. It also
        // must not condemn one: this returns Unknown, and the core turns that
        // into "ask me again", not into a deadline that has passed.
        return EvaluateRfc3339Deadline(deadline, now_millis, 1704067200000LL);
    }
    uint64_t MonotonicTime() const override {
        return static_cast<uint64_t>(esp_timer_get_time() / 1000);
    }
};

class Sha256Fingerprint final : public DeviceDeliveryFingerprintPort {
public:
    bool Sha256(const std::string& canonical, std::string& digest) override {
        unsigned char raw[32];
        if (mbedtls_sha256(
                reinterpret_cast<const unsigned char*>(canonical.data()),
                canonical.size(), raw, 0) != 0) {
            return false;
        }
        static constexpr char hex[] = "0123456789abcdef";
        digest = "sha256:";
        digest.reserve(71);
        for (const unsigned char byte : raw) {
            digest.push_back(hex[byte >> 4]);
            digest.push_back(hex[byte & 0x0f]);
        }
        return true;
    }
};

esp_err_t StatusError(int status) {
    if (status == 401 || status == 403) return ESP_ERR_NOT_ALLOWED;
    if (status == 404) return ESP_ERR_NOT_FOUND;
    if (status == 409 || status == 422) return ESP_ERR_INVALID_STATE;
    if (status == 410) return ESP_ERR_TIMEOUT;
    return ESP_FAIL;
}

}  // namespace

esp_err_t DeviceControlDeliveryClient::PollAndExecute(
    const ActiveClaimState& claim,
    const OwnerTrustBundle& trust,
    bool& removal_completed) {
    removal_completed = false;
    if (!claim.valid() || claim.state != ActiveClaimLocalState::Active) {
        return ESP_ERR_INVALID_STATE;
    }
    auto& identity = DeviceIdentity::GetInstance();
    esp_err_t err = identity.EnsureKeypair();
    if (err != ESP_OK) return err;
    const std::string nonce = RandomNonce();
    const std::string proof =
        std::string("{\"device_ref\":") + DeviceRefJson(claim.device_ref) +
        ",\"nonce\":" + Quote(nonce) +
        ",\"operation_type\":\"device-local.erase\"}";
    std::string signature;
    err = identity.SignCanonical(proof, signature);
    if (err != ESP_OK || nonce.empty() || signature.size() != 86) return ESP_FAIL;

    const std::string request =
        std::string("{\"device_ref\":") + DeviceRefJson(claim.device_ref) +
        ",\"device_signature\":" + Quote(signature) +
        ",\"nonce\":" + Quote(nonce) +
        ",\"public_key_spki\":" + Quote(identity.DeviceControlPublicKey()) + "}";
    device_foundation::v1::AuthorityEndpoint endpoint;
    err = DeviceAuthorityLocator::GetInstance().Resolve(
        device_foundation::v1::LogicalAuthority::DeviceControl, endpoint);
    if (err != ESP_OK) return err;
    HubHttpResponse response;
    err = HubHttpRequest(
        "POST", endpoint.uri + "/erase-operations:pull",
        trust.owner_root_certificate_pem, request, response);
    if (err != ESP_OK) return err;
    if (response.status == 204) return ESP_OK;
    if (response.status != 200) return StatusError(response.status);

    EspIdfDeviceEraseJournal journal;
    EspIdfOwnerDataEraseStorage storage;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    OperationalClock clock;
    DeviceIdentityEraseAckSigner signer;
    Sha256Fingerprint fingerprint;
    DeviceLocalEraseCore erase(
        claim.device_ref, journal, adapter, clock, signer);
    DeviceDeliveryConsumerCore delivery(erase, fingerprint);
    const DeviceDeliveryConsumerOutcome outcome = delivery.Handle(response.body);
    if (!outcome.has_evidence) {
        if (outcome.result == DeviceDeliveryConsumerResult::AcceptedWithoutEvidence) {
            return ESP_OK;
        }
        // The core already decided why, in one word, and this used to throw it
        // away. An Owner instruction that cannot be executed then looked like
        // an unexplained ESP_FAIL, while the Authority went on re-arming a
        // delivery nobody could see being refused.
        ESP_LOGW(TAG, "Owner instruction refused: %s (attempt %s)",
                 outcome.acceptance.adapter_code.c_str(),
                 outcome.acceptance.delivery_attempt_id.c_str());
        return ESP_FAIL;
    }

    HubHttpResponse acknowledged;
    err = HubHttpRequest(
        "POST", endpoint.uri + "/erase-operations/" +
                    outcome.evidence.message_id + "/ack",
        trust.owner_root_certificate_pem, outcome.evidence_json, acknowledged);
    if (err != ESP_OK) return err;
    if (acknowledged.status != 200) return StatusError(acknowledged.status);
    removal_completed = true;
    return ESP_OK;
}

}  // namespace eidolon
