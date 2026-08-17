#include "hub_onboarding_client.h"

#include "board.h"
#include "device_provisioning_protocol.h"
#include "hub_config_store.h"
#include "hub_onboarding_protocol.h"
#include "hub_pinned_http.h"
#include "hub_trust_store.h"
#include "system_info.h"

#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>

#include <cstring>
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

std::string BuildEnrollmentBody(const HubOnboardingState& state)
{
    // Ask the board itself rather than assuming: a build with no camera must
    // not offer one, or the Host provisions a video channel nobody publishes to.
    const bool has_camera = Board::GetInstance().GetCamera() != nullptr;
    const std::string manifest_json = BuildDeviceManifestJson(BOARD_NAME, has_camera);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "operation", "device.enrollment");
    cJSON_AddStringToObject(root, "request_id", state.request_id.c_str());
    cJSON_AddStringToObject(root, "retrieval_token", state.retrieval_token.c_str());
    cJSON* identity = cJSON_AddObjectToObject(root, "identity");
    cJSON_AddStringToObject(identity, "device_id", state.device_id.c_str());
    cJSON_AddItemToObject(root, "manifest",
                         cJSON_ParseWithLength(manifest_json.data(), manifest_json.size()));
    cJSON_AddStringToObject(root, "display_name", BOARD_NAME);
    cJSON_AddStringToObject(root, "device_kind", BOARD_TYPE);
    const std::string body = PrintJson(root);
    cJSON_Delete(root);
    return body;
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

esp_err_t HubOnboardingClient::FetchDescriptor(const HubTxtRecord& advertised,
                                               HubDescriptor& out)
{
    HubHttpResponse response;
    const esp_err_t err = HubHttpRequest("GET", advertised.descriptor_uri, certificate_,
                                         "", response);
    if (err != ESP_OK) {
        return err;
    }
    if (response.status != 200) {
        ESP_LOGW(TAG, "Descriptor HTTP status %d", response.status);
        return StatusError(response.status);
    }
    if (!ParseHubDescriptorResponse(response.body, advertised, out)) {
        ESP_LOGE(TAG, "Hub descriptor violates advertised onboarding contract");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t HubOnboardingClient::EnsureEnrollment(const HubDescriptor& descriptor,
                                                HubOnboardingState& state)
{
    if (state.enrolled()) {
        return ESP_OK;
    }
    const std::string body = BuildEnrollmentBody(state);
    if (body.empty()) {
        return ESP_ERR_NO_MEM;
    }
    HubHttpResponse response;
    const esp_err_t err = HubHttpRequest("POST", descriptor.enrollment_uri, certificate_,
                                         body, response);
    if (err != ESP_OK) {
        return err;
    }
    if (response.status != 200) {
        ESP_LOGW(TAG, "Enrollment HTTP status %d", response.status);
        return StatusError(response.status);
    }
    HubEnrollmentReceipt receipt;
    if (!ParseEnrollmentReceiptResponse(response.body, state, receipt)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    state.enrollment_id = receipt.enrollment_id;
    state.lifecycle_state = receipt.lifecycle_state;
    return HubConfigStore().SaveOnboardingState(state);
}

esp_err_t HubOnboardingClient::Handoff(const HubDescriptor& descriptor,
                                       HubOnboardingState& state,
                                       Esp32HubConfig& out)
{
    const std::string request_id = "handoff-" + Base64UrlRandom(16);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "operation", "device.handoff");
    cJSON_AddStringToObject(root, "request_id", request_id.c_str());
    cJSON_AddStringToObject(root, "retrieval_token", state.retrieval_token.c_str());
    std::string body = PrintJson(root);
    cJSON_Delete(root);
    if (request_id.size() <= std::strlen("handoff-") || body.empty()) {
        return ESP_ERR_NO_MEM;
    }
    const std::string url = descriptor.enrollment_uri + "/" + state.enrollment_id +
                            "/handoff";
    HubHttpResponse response;
    const esp_err_t err = HubHttpRequest("POST", url, certificate_, body, response);
    if (err != ESP_OK) {
        return err;
    }
    if (response.status != 200 && response.status != 202) {
        ESP_LOGW(TAG, "Handoff HTTP status %d", response.status);
        return StatusError(response.status);
    }
    HubConfigStatus lifecycle = HubConfigStatus::PendingApproval;
    HubChannelAssignment assignment;
    if (!ParseHandoffResponse(response.body, request_id, state, lifecycle, assignment)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out = Esp32HubConfig{};
    out.status = lifecycle;
    state.lifecycle_state = HubConfigStatusToString(lifecycle);
    if (lifecycle == HubConfigStatus::Active) {
        std::string binding;
        if (!Base64Decode(assignment.opaque_binding, binding) ||
            !ParseLiveKitBinding(binding, out)) {
            ESP_LOGE(TAG, "Provider returned an invalid %s binding", kLiveKitBindingFormat);
            return ESP_ERR_INVALID_RESPONSE;
        }
        out.expires_at_ms = assignment.expires_at_ms;
        // The opaque provider credential is consumed in RAM only. Hub never logs
        // or persists it; the device config store atomically caches the parsed
        // credential for bounded offline recovery.
    }
    return HubConfigStore().SaveOnboardingState(state);
}

esp_err_t HubOnboardingClient::LoadCommissionedTrust()
{
    HubTrustStore trust;
    commissioned_hub_id_ = trust.CommissionedHubId();
    certificate_ = trust.Load(commissioned_hub_id_);
    if (commissioned_hub_id_.empty() || certificate_.empty()) {
        ESP_LOGE(TAG, "This device has not been commissioned for any Host");
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t HubOnboardingClient::Run(const HubTxtRecord& advertised,
                                   const std::string& device_id,
                                   Esp32HubConfig& out)
{
    esp_err_t err = LoadCommissionedTrust();
    if (err != ESP_OK) {
        return err;
    }
    HubDescriptor descriptor;
    err = FetchDescriptor(advertised, descriptor);
    if (err != ESP_OK) {
        return err;
    }
    if (!IsCommissionedHub(commissioned_hub_id_, descriptor.hub_id)) {
        // Discovery found a Hub, but not the one this device was given. Trust
        // came from a person, so a different Hub is not a fallback.
        ESP_LOGE(TAG, "Discovered Hub %s is not the commissioned Host %s",
                 descriptor.hub_id.c_str(), commissioned_hub_id_.c_str());
        return ESP_ERR_NOT_ALLOWED;
    }
    HubConfigStore store;
    bool restarted_expired_pending = false;
    bool restarted_after_revocation = false;
    for (;;) {
        HubOnboardingState state;
        const bool has_saved_state = store.LoadOnboardingState(state);
        if (has_saved_state &&
            (state.hub_id != descriptor.hub_id || state.device_id != device_id)) {
            ESP_LOGE(TAG, "Refusing automatic onboarding state switch to another Hub or device");
            return ESP_ERR_NOT_ALLOWED;
        }
        const bool reusable = has_saved_state && state.has_local_intent();
        if (!reusable) {
            store.ClearOnboardingState();
            state = HubOnboardingState{};
            state.hub_id = descriptor.hub_id;
            state.descriptor_uri = descriptor.descriptor_uri;
            state.enrollment_uri = descriptor.enrollment_uri;
            state.device_id = device_id;
            state.request_id = "enroll-" + Base64UrlRandom(16);
            state.retrieval_token = Base64UrlRandom(32);
            if (!state.has_local_intent()) {
                return ESP_ERR_NO_MEM;
            }
            err = store.SaveOnboardingState(state);
            if (err != ESP_OK) {
                return err;
            }
        } else if (state.descriptor_uri != descriptor.descriptor_uri ||
                   state.enrollment_uri != descriptor.enrollment_uri) {
            // The Hub identity is pinned by hub_id. URI changes for that same Hub
            // reuse the persisted idempotency and retrieval material.
            state.descriptor_uri = descriptor.descriptor_uri;
            state.enrollment_uri = descriptor.enrollment_uri;
            err = store.SaveOnboardingState(state);
            if (err != ESP_OK) {
                return err;
            }
        }
        err = EnsureEnrollment(descriptor, state);
        if (err != ESP_OK) {
            return err;
        }
        err = Handoff(descriptor, state, out);
        if (err == ESP_OK && state.lifecycle_state == "revoked" &&
            !restarted_after_revocation) {
            // The Owner took this device off the Host. The Hub permits a revoked
            // device to enrol from scratch, and says why: that is the only way one
            // that was removed, or whose Host was reinstalled, ever comes back.
            // Without asking again the device sat repeating "authorization
            // required" while appearing in no list the Owner could approve from —
            // the grant was gone and nothing was requesting a new one.
            //
            // Asking is not being granted. A fresh enrolment lands in
            // pending-approval, where the Owner decides as they did the first time.
            ESP_LOGW(TAG, "Host revoked this device; asking to enrol again");
            store.ClearOnboardingState();
            restarted_after_revocation = true;
            continue;
        }
        if (err != ESP_ERR_TIMEOUT || state.lifecycle_state != "pending-approval" ||
            restarted_expired_pending) {
            return err;
        }
        // A pending intent may be recreated exactly once after Hub reports that
        // its retrieval window expired. Never recurse or re-enroll an approved
        // device: Owner admission remains authoritative.
        store.ClearOnboardingState();
        restarted_expired_pending = true;
    }
}

esp_err_t HubOnboardingClient::Resume(const std::string& descriptor_uri,
                                      const std::string& device_id,
                                      Esp32HubConfig& out)
{
    HubOnboardingState state;
    if (!HubConfigStore().LoadOnboardingState(state) ||
        state.descriptor_uri != descriptor_uri || state.device_id != device_id) {
        return ESP_ERR_NOT_FOUND;
    }
    HubTxtRecord advertised;
    advertised.txtvers = kSupportedTxtVers;
    advertised.descriptor_uri = descriptor_uri;
    advertised.enrollment_uri = state.enrollment_uri;
    return Run(advertised, device_id, out);
}

}  // namespace eidolon
