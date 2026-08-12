#include "device_commissioning.h"

#include "device_commissioning_protocol.h"
#include "hub_trust_store.h"
#include "system_info.h"

#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstring>
#include <memory>

#define TAG "Commissioning"

namespace eidolon {

namespace {

// Refused before the body is read; the parser applies the same bound to what
// it is given.
constexpr size_t kMaxPayloadBytes = 8 * 1024;

// The vendor captive portal owns port 80 on this access point. Commissioning is
// our own contract, so it gets its own port rather than a patch to a component
// the build re-resolves from upstream.
constexpr int kPort = 8266;

esp_err_t SendJson(httpd_req_t* request, const char* status, const std::string& body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t SendError(httpd_req_t* request, const char* status, const char* detail)
{
    std::string body = "{\"schema_version\":1,\"error\":\"";
    body += detail;
    body += "\"}";
    return SendJson(request, status, body);
}

}  // namespace

int DeviceCommissioningServer::Port()
{
    return kPort;
}

DeviceCommissioningServer::~DeviceCommissioningServer()
{
    Stop();
}

esp_err_t DeviceCommissioningServer::Start(WifiJoin join)
{
    if (server_ != nullptr) {
        return ESP_OK;
    }
    join_ = std::move(join);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    // The captive portal already holds the default control port.
    config.ctrl_port = config.ctrl_port + 1;
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    // Parsing a certificate payload and committing it to NVS does not fit in
    // the default handler stack, and overrunning it takes the whole device down
    // in the middle of being set up.
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Commissioning server failed to start: %s", esp_err_to_name(err));
        server_ = nullptr;
        return err;
    }

    const httpd_uri_t identity = {
        .uri = "/identity",
        .method = HTTP_GET,
        .handler = &DeviceCommissioningServer::HandleIdentity,
        .user_ctx = this,
    };
    const httpd_uri_t commission = {
        .uri = "/commission",
        .method = HTTP_POST,
        .handler = &DeviceCommissioningServer::HandleCommission,
        .user_ctx = this,
    };
    if (httpd_register_uri_handler(server_, &identity) != ESP_OK ||
        httpd_register_uri_handler(server_, &commission) != ESP_OK) {
        Stop();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Commissioning endpoint listening on port %d", kPort);
    return ESP_OK;
}

void DeviceCommissioningServer::Stop()
{
    if (server_ == nullptr) {
        return;
    }
    httpd_stop(server_);
    server_ = nullptr;
    join_ = nullptr;
}

esp_err_t DeviceCommissioningServer::HandleIdentity(httpd_req_t* request)
{
    // The commissioner needs to know which device it just configured so it can
    // recognize the enrollment this device is about to create.
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return SendError(request, "500 Internal Server Error", "out of memory");
    }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "board", BOARD_NAME);
    cJSON_AddStringToObject(root, "device_kind", BOARD_TYPE);
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return SendError(request, "500 Internal Server Error", "out of memory");
    }
    const std::string body = printed;
    cJSON_free(printed);
    return SendJson(request, "200 OK", body);
}

esp_err_t DeviceCommissioningServer::HandleCommission(httpd_req_t* request)
{
    auto* self = static_cast<DeviceCommissioningServer*>(request->user_ctx);
    const size_t length = request->content_len;
    if (length == 0 || length > kMaxPayloadBytes) {
        return SendError(request, "400 Bad Request", "payload size is not supported");
    }

    std::string raw(length, '\0');
    size_t received = 0;
    while (received < length) {
        const int read = httpd_req_recv(request, raw.data() + received, length - received);
        if (read <= 0) {
            return SendError(request, "400 Bad Request", "payload was not received");
        }
        received += static_cast<size_t>(read);
    }

    CommissioningIntent intent;
    if (!ParseCommissioningIntent(raw, intent)) {
        return SendError(request, "400 Bad Request", "commissioning payload is incomplete");
    }

    // Trust first: a device that joins the network without knowing which Host
    // it belongs to has nothing to do there.
    HubTrustStore trust;
    const esp_err_t saved = trust.Save(intent.hub_id, intent.certificate_pem);
    if (saved != ESP_OK) {
        ESP_LOGE(TAG, "Rejected commissioned certificate: %s", esp_err_to_name(saved));
        return SendError(request, "400 Bad Request", "certificate was rejected");
    }

    if (intent.changes_network && (!self->join_ || !self->join_(intent.ssid, intent.password))) {
        // The certificate stays: the Host it names did not become wrong because
        // the Wi-Fi credentials were. The commissioner can retry the network
        // without handing the certificate over again.
        return SendError(request, "409 Conflict", "could not join that network");
    }

    ESP_LOGI(TAG, "Commissioned for Hub %s%s%s", intent.hub_id.c_str(),
             intent.changes_network ? " on network " : " (network unchanged)",
             intent.changes_network ? intent.ssid.c_str() : "");
    std::string body = "{\"schema_version\":1,\"device_id\":\"";
    body += SystemInfo::GetMacAddress();
    body += "\",\"hub_id\":\"";
    body += intent.hub_id;
    body += "\"}";
    return SendJson(request, "200 OK", body);
}

}  // namespace eidolon
