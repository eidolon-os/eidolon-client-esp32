#include "hub_onboarding_client.h"

#include "authority_locator.h"
#include "device_identity.h"
#include "device_provisioning_protocol.h"
#include "hub_config_store.h"
#include "hub_onboarding_protocol.h"
#include "hub_pinned_http.h"
#include "hub_trust_store.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#include <cstdint>
#include <utility>

#define TAG "HubOnboarding"

namespace eidolon {

namespace {

std::string Base64UrlRandom(size_t bytes)
{
    std::string random(bytes, '\0');
    esp_fill_random(random.data(), random.size());
    size_t capacity = 4 * ((bytes + 2) / 3) + 1;
    std::string encoded(capacity, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
            &written, reinterpret_cast<const unsigned char*>(random.data()),
            random.size()) != 0) {
        return "";
    }
    encoded.resize(written);
    for (char& character : encoded) {
        if (character == '+') {
            character = '-';
        } else if (character == '/') {
            character = '_';
        }
    }
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

bool Base64Decode(const std::string& encoded, std::string& out)
{
    size_t required = 0;
    int result = mbedtls_base64_decode(
        nullptr, 0, &required,
        reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
    if (result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required == 0 ||
        required > 64 * 1024) {
        return false;
    }
    out.resize(required);
    size_t written = 0;
    result = mbedtls_base64_decode(
        reinterpret_cast<unsigned char*>(out.data()), out.size(), &written,
        reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
    if (result != 0) {
        out.clear();
        return false;
    }
    out.resize(written);
    return !out.empty();
}

std::string PrintJson(cJSON* root)
{
    char* encoded = root ? cJSON_PrintUnformatted(root) : nullptr;
    std::string out = encoded ? encoded : "";
    if (encoded != nullptr) {
        cJSON_free(encoded);
    }
    return out;
}

std::string Quote(const std::string& value)
{
    cJSON* item = cJSON_CreateString(value.c_str());
    const std::string encoded = PrintJson(item);
    cJSON_Delete(item);
    return encoded;
}

std::string DeviceRefCanonicalJson(
    const device_foundation::v1::DeviceRef& ref)
{
    return std::string("{\"claim_generation\":") +
           std::to_string(ref.claim_generation) +
           ",\"device_instance_id\":" + Quote(ref.device_instance_id) +
           ",\"owner_domain_generation\":" +
           std::to_string(ref.owner_domain_generation) +
           ",\"owner_domain_id\":" + Quote(ref.owner_domain_id.value) +
           ",\"trust_epoch\":" + std::to_string(ref.trust_epoch) + "}";
}

std::string ConfigurationProofDocument(
    const device_foundation::v1::DeviceRef& ref,
    const std::string& nonce)
{
    return std::string("{\"device_ref\":") + DeviceRefCanonicalJson(ref) +
           ",\"nonce\":" + Quote(nonce) +
           ",\"operation_type\":\"device-control.configuration\"}";
}

cJSON* DeviceRefJson(const device_foundation::v1::DeviceRef& ref)
{
    cJSON* value = cJSON_CreateObject();
    if (!value) return nullptr;
    cJSON_AddStringToObject(value, "device_instance_id",
                            ref.device_instance_id.c_str());
    cJSON_AddStringToObject(value, "owner_domain_id",
                            ref.owner_domain_id.value.c_str());
    cJSON_AddNumberToObject(value, "owner_domain_generation",
                            static_cast<double>(ref.owner_domain_generation));
    cJSON_AddNumberToObject(value, "claim_generation", ref.claim_generation);
    cJSON_AddNumberToObject(value, "trust_epoch", ref.trust_epoch);
    return value;
}

esp_err_t StatusError(int status)
{
    if (status == 401 || status == 403) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status == 404) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status == 409 || status == 422) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == 410) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_FAIL;
}

}  // namespace

esp_err_t HubOnboardingClient::FetchDescriptor(
    const AuthorityCandidateRecord& candidate,
    device_foundation::v1::OwnerDomainDescriptor& out)
{
    HubHttpResponse response;
    const esp_err_t err = HubHttpRequest(
        "GET", candidate.owner_domain_descriptor_uri,
        trust_.owner_root_certificate_pem, "", response);
    if (err != ESP_OK) {
        return err;
    }
    if (response.status != 200) {
        ESP_LOGW(TAG, "Descriptor HTTP status %d", response.status);
        return StatusError(response.status);
    }
    std::string canonical;
    if (!ParseOwnerDomainDescriptor(response.body, out, canonical)) {
        ESP_LOGE(TAG, "Owner Domain descriptor is not a V1 contract document");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const esp_err_t accepted =
        DeviceAuthorityLocator::GetInstance().AcceptDescriptor(
            out, canonical, response.body);
    if (accepted != ESP_OK) {
        ESP_LOGE(TAG, "Owner Domain descriptor failed trust or revision checks");
        return accepted;
    }
    const esp_err_t loaded =
        DeviceAuthorityLocator::GetInstance().AcceptedDescriptor(out);
    if (loaded == ESP_OK) {
        const uint32_t revision_high =
            static_cast<uint32_t>(out.directory_revision >> 32);
        const uint32_t revision_low =
            static_cast<uint32_t>(out.directory_revision);
        if (revision_high == 0) {
            ESP_LOGI(TAG, "Accepted Owner directory owner=%s revision=%lu",
                     out.owner_domain_id.c_str(),
                     static_cast<unsigned long>(revision_low));
        } else {
            // ESP-IDF's nano formatter does not support %llu. Two fixed-width
            // halves preserve the exact 64-bit revision in device evidence.
            ESP_LOGI(TAG,
                     "Accepted Owner directory owner=%s revision=0x%08lx%08lx",
                     out.owner_domain_id.c_str(),
                     static_cast<unsigned long>(revision_high),
                     static_cast<unsigned long>(revision_low));
        }
    }
    return loaded;
}

esp_err_t HubOnboardingClient::PullActiveConfiguration(
    const ActiveClaimState& claim,
    Esp32HubConfig& out)
{
    if (!claim.valid()) return ESP_ERR_INVALID_STATE;
    auto& identity = DeviceIdentity::GetInstance();
    esp_err_t err = identity.EnsureKeypair();
    if (err != ESP_OK) return err;
    const std::string nonce = Base64UrlRandom(18);
    const std::string public_key = identity.PublicKeySpki();
    std::string signature;
    err = identity.SignCanonical(
        ConfigurationProofDocument(claim.device_ref, nonce), signature);
    if (err != ESP_OK || nonce.empty() || public_key.empty() ||
        signature.size() != 86) {
        return ESP_FAIL;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON* device_ref = DeviceRefJson(claim.device_ref);
    if (!root || !device_ref) {
        cJSON_Delete(root);
        cJSON_Delete(device_ref);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "device_ref", device_ref);
    cJSON_AddStringToObject(root, "nonce", nonce.c_str());
    cJSON_AddStringToObject(root, "public_key_spki", public_key.c_str());
    cJSON_AddStringToObject(root, "device_signature", signature.c_str());
    const std::string body = PrintJson(root);
    cJSON_Delete(root);
    if (body.empty()) return ESP_ERR_NO_MEM;

    device_foundation::v1::AuthorityEndpoint control;
    err = DeviceAuthorityLocator::GetInstance().Resolve(
        device_foundation::v1::LogicalAuthority::DeviceControl, control);
    if (err != ESP_OK) return err;
    HubHttpResponse response;
    err = HubHttpRequest(
        "POST", control.uri + "/configuration:pull",
        trust_.owner_root_certificate_pem, body, response);
    if (err != ESP_OK) return err;
    if (response.status != 200) return StatusError(response.status);
    HubConfigStatus status = HubConfigStatus::PendingApproval;
    HubChannelAssignment assignment;
    if (!ParseDeviceConfigurationResponse(
            response.body, nonce, claim, status, assignment)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out = Esp32HubConfig{};
    out.status = status;
    if (status == HubConfigStatus::Active) {
        std::string binding;
        if (!Base64Decode(assignment.opaque_binding, binding) ||
            !ParseLiveKitBinding(binding, out)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        out.expires_at_ms = assignment.expires_at_ms;
    }
    if (status == HubConfigStatus::Revoked) {
        ActiveClaimState revoked = claim;
        revoked.state = ActiveClaimLocalState::Revoked;
        if (!HubConfigStore().StoreActiveClaim(revoked)) {
            return ESP_FAIL;
        }
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t HubOnboardingClient::LoadCommissionedTrust()
{
    auto& locator = DeviceAuthorityLocator::GetInstance();
    if (locator.ReloadCommissionedDirectory() != ESP_OK ||
        locator.TrustBundle(trust_) != ESP_OK) {
        ESP_LOGE(TAG, "This device has no valid commissioned Owner Domain");
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t HubOnboardingClient::Run(const AuthorityCandidateRecord& candidate,
                                   const std::string& device_id,
                                   Esp32HubConfig& out)
{
    esp_err_t err = LoadCommissionedTrust();
    if (err != ESP_OK) return err;
    if (!IsCommissionedOwnerDomain(
            trust_.owner_domain_id, candidate.owner_domain_id)) {
        ESP_LOGE(TAG, "Discovery candidate is for another Owner Domain");
        return ESP_ERR_NOT_ALLOWED;
    }
    device_foundation::v1::OwnerDomainDescriptor descriptor;
    err = FetchDescriptor(candidate, descriptor);
    if (err != ESP_OK) return err;
    return RunAccepted(descriptor, device_id, out);
}

esp_err_t HubOnboardingClient::RunAccepted(
    const device_foundation::v1::OwnerDomainDescriptor& descriptor,
    const std::string& device_id,
    Esp32HubConfig& out)
{
    HubConfigStore store;
    ActiveClaimState active_claim;
    const ClaimStoreLoadResult claim_load =
        store.LoadActiveClaim(active_claim);
    if (claim_load == ClaimStoreLoadResult::StorageFailure) {
        ESP_LOGE(TAG, "ActiveClaimStore is unreadable; refusing runtime");
        return ESP_FAIL;
    }
    if (claim_load == ClaimStoreLoadResult::Loaded) {
        if (active_claim.state == ActiveClaimLocalState::Revoked) {
            ESP_LOGW(TAG, "Claim is terminal revoked; physical recovery required");
            return ESP_ERR_NOT_ALLOWED;
        }
        if (active_claim.device_ref.device_instance_id != device_id ||
            active_claim.device_ref.owner_domain_id.value !=
                descriptor.owner_domain_id ||
            active_claim.device_ref.owner_domain_generation !=
                descriptor.owner_domain_generation) {
            ESP_LOGE(TAG, "Recovery required: active Claim Authority changed");
            return ESP_ERR_NOT_ALLOWED;
        }
        return PullActiveConfiguration(active_claim, out);
    }
    // PH2-B deliberately removes the legacy retrieval-token/handoff writer.
    // Canonical Proposal/Grant/Ack state and the SDK ClaimGrantWireEnvelope/AAD
    // boundary are implemented by DeviceClaimConsumerCore. A production HPKE
    // ClaimGrantCryptoPort + HTTP adapter is still intentionally absent, so
    // starting a new Claim remains a hard capability block, never a fallback
    // to the stale protocol.
    ESP_LOGE(TAG,
             "Canonical Claim collection wire authentication is unavailable");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t HubOnboardingClient::Resume(const std::string& device_id,
                                      Esp32HubConfig& out)
{
    esp_err_t err = LoadCommissionedTrust();
    if (err != ESP_OK) return err;
    device_foundation::v1::OwnerDomainDescriptor descriptor;
    err = DeviceAuthorityLocator::GetInstance().AcceptedDescriptor(descriptor);
    if (err != ESP_OK) return err;
    return RunAccepted(descriptor, device_id, out);
}

}  // namespace eidolon
