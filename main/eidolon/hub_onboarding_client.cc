#include "hub_onboarding_client.h"

#include "board.h"
#include "device_identity.h"
#include "hub_config_store.h"
#include "hub_onboarding_protocol.h"
#include "system_info.h"
#include "display.h"

#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <utility>

#define TAG "HubOnboarding"

namespace eidolon {

namespace {

std::string Hex(const unsigned char* data, size_t size)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

std::string Sha256(const std::string& value)
{
    unsigned char digest[32] = {};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()),
                   value.size(), digest, 0);
    return "sha256:" + Hex(digest, sizeof(digest));
}

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

std::string BuildEnrollmentBody(const HubOnboardingState& state,
                                const std::string& public_key,
                                const std::string& signature)
{
    const std::string manifest_json = BuildDeviceManifestJson(BOARD_NAME);
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
    cJSON* proof = cJSON_AddObjectToObject(root, "identity_proof");
    cJSON_AddStringToObject(proof, "algorithm", "p256-sha256");
    cJSON_AddStringToObject(proof, "public_key_spki", public_key.c_str());
    cJSON_AddStringToObject(proof, "signature", signature.c_str());
    cJSON* pairing = cJSON_AddObjectToObject(root, "pairing_proof");
    cJSON_AddStringToObject(pairing, "method", kPairingMethod);
    cJSON_AddStringToObject(pairing, "commitment", state.pairing_commitment.c_str());
    const std::string body = PrintJson(root);
    cJSON_Delete(root);
    return body;
}

void SetJsonHeaders(Http* http)
{
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
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

void UpdatePairingDisplay(const HubOnboardingState& state,
                          HubConfigStatus lifecycle)
{
    Display* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    if (lifecycle != HubConfigStatus::PendingApproval) {
        display->SetPairingCode(nullptr);
        return;
    }
    const std::string payload = BuildPairingQrPayload(state);
    if (payload.empty()) {
        ESP_LOGE(TAG, "Pending enrollment cannot be encoded as pairing QR");
        display->SetPairingCode(nullptr);
        return;
    }
    if (!display->SetPairingCode(payload.c_str())) {
        ESP_LOGW(TAG, "Display has no physical pairing-proof surface");
    }
}

}  // namespace

esp_err_t HubOnboardingClient::FetchDescriptor(const HubTxtRecord& advertised,
                                               HubDescriptor& out)
{
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(CONFIG_EIDOLON_CONFIG_HTTP_TIMEOUT_MS) : nullptr;
    if (!http) {
        return network ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    if (!http->Open("GET", advertised.descriptor_uri)) {
        return ESP_FAIL;
    }
    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGW(TAG, "Descriptor HTTP status %d", status);
        return StatusError(status);
    }
    if (!ParseHubDescriptorResponse(body, advertised, out)) {
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
    DeviceIdentity& identity = DeviceIdentity::GetInstance();
    const std::string manifest = BuildDeviceManifestJson(BOARD_NAME);
    const std::string statement = BuildEnrollmentProofStatement(
        state.request_id, state.device_id, Sha256(state.retrieval_token),
        state.pairing_commitment, BOARD_TYPE, BOARD_NAME, Sha256(manifest));
    std::string public_key;
    std::string signature;
    esp_err_t err = identity.SignEnrollmentProof(statement, public_key, signature);
    if (err != ESP_OK) {
        return err;
    }
    std::string body = BuildEnrollmentBody(state, public_key, signature);
    if (body.empty()) {
        return ESP_ERR_NO_MEM;
    }
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(CONFIG_EIDOLON_CONFIG_HTTP_TIMEOUT_MS) : nullptr;
    if (!http) {
        return network ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    SetJsonHeaders(http.get());
    http->SetContent(std::move(body));
    if (!http->Open("POST", descriptor.enrollment_uri)) {
        return ESP_FAIL;
    }
    const int status = http->GetStatusCode();
    const std::string response = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGW(TAG, "Enrollment HTTP status %d", status);
        return StatusError(status);
    }
    HubEnrollmentReceipt receipt;
    if (!ParseEnrollmentReceiptResponse(response, state, receipt)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    state.enrollment_id = receipt.enrollment_id;
    state.pairing_claim_uri = receipt.pairing_claim_uri;
    state.lifecycle_state = receipt.lifecycle_state;
    state.retrieval_expires_at_ms = receipt.retrieval_expires_at_ms;
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
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(CONFIG_EIDOLON_CONFIG_HTTP_TIMEOUT_MS) : nullptr;
    if (!http) {
        return network ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    SetJsonHeaders(http.get());
    http->SetContent(std::move(body));
    if (!http->Open("POST", url)) {
        return ESP_FAIL;
    }
    const int http_status = http->GetStatusCode();
    const std::string response = http->ReadAll();
    http->Close();
    if (http_status != 200 && http_status != 202) {
        ESP_LOGW(TAG, "Handoff HTTP status %d", http_status);
        return StatusError(http_status);
    }
    HubConfigStatus lifecycle = HubConfigStatus::PendingApproval;
    HubChannelAssignment assignment;
    if (!ParseHandoffResponse(response, request_id, state, lifecycle, assignment)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out = Esp32HubConfig{};
    out.status = lifecycle;
    out.device_fingerprint = DeviceIdentity::GetInstance().Fingerprint();
    state.lifecycle_state = HubConfigStatusToString(lifecycle);
    if (lifecycle == HubConfigStatus::Active) {
        std::string binding;
        if (!Base64Decode(assignment.opaque_binding, binding) ||
            !ParseLiveKitBinding(binding, out)) {
            ESP_LOGE(TAG, "Provider returned an invalid %s binding", kLiveKitBindingFormat);
            return ESP_ERR_INVALID_RESPONSE;
        }
        out.registration_id = assignment.channel_id;
        // The opaque provider credential is consumed in RAM only. Hub never logs
        // or persists it; the device config store atomically caches the parsed
        // credential for bounded offline recovery.
    }
    UpdatePairingDisplay(state, lifecycle);
    if (lifecycle != HubConfigStatus::PendingApproval) {
        // The Owner proof is one-purpose admission material. Keep the retrieval
        // session for handoff refresh, but erase the plaintext proof once Hub no
        // longer reports an approvable pending enrollment.
        state.pairing_secret.clear();
        state.pairing_commitment.clear();
    }
    return HubConfigStore().SaveOnboardingState(state);
}

esp_err_t HubOnboardingClient::Run(const HubTxtRecord& advertised,
                                   const std::string& device_id,
                                   Esp32HubConfig& out)
{
    HubDescriptor descriptor;
    esp_err_t err = FetchDescriptor(advertised, descriptor);
    if (err != ESP_OK) {
        return err;
    }
    HubConfigStore store;
    HubOnboardingState state;
    const bool has_saved_state = store.LoadOnboardingState(state);
    if (has_saved_state &&
        (state.hub_id != descriptor.hub_id || state.device_id != device_id)) {
        ESP_LOGE(TAG, "Refusing automatic onboarding state switch to another Hub or device");
        return ESP_ERR_NOT_ALLOWED;
    }
    const bool reusable = has_saved_state && state.resumable();
    if (!reusable) {
        store.ClearOnboardingState();
        state = HubOnboardingState{};
        state.hub_id = descriptor.hub_id;
        state.descriptor_uri = descriptor.descriptor_uri;
        state.enrollment_uri = descriptor.enrollment_uri;
        state.device_id = device_id;
        state.request_id = "enroll-" + Base64UrlRandom(16);
        state.retrieval_token = Base64UrlRandom(32);
        state.pairing_secret = Base64UrlRandom(32);
        state.pairing_commitment = Sha256(state.pairing_secret);
        if (!state.has_local_intent() ||
            store.SaveOnboardingState(state) != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
    } else if (state.descriptor_uri != descriptor.descriptor_uri ||
               state.enrollment_uri != descriptor.enrollment_uri) {
        // The Hub identity is pinned by hub_id. URI changes for that same Hub
        // reuse the persisted idempotency and retrieval material.
        state.descriptor_uri = descriptor.descriptor_uri;
        state.enrollment_uri = descriptor.enrollment_uri;
        if (store.SaveOnboardingState(state) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    err = EnsureEnrollment(descriptor, state);
    if (err != ESP_OK) {
        return err;
    }
    err = Handoff(descriptor, state, out);
    if (err == ESP_ERR_TIMEOUT && state.lifecycle_state == "pending-approval") {
        // Hub expired an unapproved intent. Drop only that intent and retry
        // once with new request/retrieval/pairing material.
        UpdatePairingDisplay(state, HubConfigStatus::Unregistered);
        store.ClearOnboardingState();
        return Run(advertised, device_id, out);
    }
    return err;
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
