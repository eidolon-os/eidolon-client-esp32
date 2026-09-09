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
#include "mbedtls_compat.h"
#include <mbedtls/base64.h>

#include <cstdint>
#include <cstdio>

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

// The clock the Owner's instruction is judged against: the Authority's own, as
// stated by the very response that delivered the instruction.
//
// Reading `gettimeofday` here is what turned a retryable refusal into a
// permanent one. A HUB_MODE build never sets the system clock — the Xiaozhi
// version check is the only caller of `settimeofday` in this firmware and that
// path is compiled out — so the local wall clock answers 1970 for the whole
// life of the boot, on every boot. "Ask me again once time is trusted" was
// therefore waiting for something that was never going to arrive, and a
// week-long deadline stayed unreadable until the Hub gave up. The time is
// collected in the same round trip as the command instead, which leaves no
// window between learning it and using it.
class OperationalClock final : public DeviceEraseClockPort {
public:
    explicit OperationalClock(int64_t hub_utc_millis)
        : hub_utc_millis_(hub_utc_millis) {}

    Rfc3339DeadlineState DeadlineState(
        const std::string& deadline) const override {
        // An untrusted clock must not authorize a destructive command. It also
        // must not condemn one: a Hub that stated no readable time leaves this
        // Unknown, and the core turns that into "ask me again", not into a
        // deadline that has passed.
        return EvaluateRfc3339Deadline(deadline, hub_utc_millis_,
                                       1704067200000LL);
    }
    uint64_t MonotonicTime() const override {
        return static_cast<uint64_t>(esp_timer_get_time() / 1000);
    }

private:
    int64_t hub_utc_millis_ = 0;
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
    bool& removal_completed, const std::function<bool()>& current) {
    removal_completed = false;
    // A revoked Claim is still this device's own Claim, and revoked is the one
    // state in which an erase instruction is likely to exist. Requiring Active
    // here refused the question in exactly the situation it exists to ask: the
    // caller that reaches this on the removal path holds a Claim it has already
    // recorded as revoked, so every consult from a removed device answered
    // ESP_ERR_INVALID_STATE without a request ever leaving the board. What this
    // device may ask about is decided by MayConsultOwnerInstruction, which
    // admits a revoked Claim and says why; what it may be told is decided by
    // the Authority, against the operational key this request is signed with.
    // The Claim still has to be a readable one, which `valid()` covers for both
    // states.
    if (!current || !current() || !claim.valid()) {
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
    if (!current()) return ESP_ERR_INVALID_STATE;
    if (response.status == 204) return ESP_OK;
    if (response.status != 200) return StatusError(response.status);

    EspIdfDeviceEraseJournal journal;
    EspIdfOwnerDataEraseStorage storage;
    EspIdfDeviceLocalEraseAdapter adapter(storage);
    // The deadline this device is about to judge was written by the Hub that
    // just answered, and the answer says when that was.
    OperationalClock clock(response.hub_utc_millis);
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
        ESP_LOGW(TAG, "Owner instruction refused: %s (attempt %s, hub time %lld)",
                 outcome.acceptance.adapter_code.c_str(),
                 outcome.acceptance.delivery_attempt_id.c_str(),
                 static_cast<long long>(response.hub_utc_millis));
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
