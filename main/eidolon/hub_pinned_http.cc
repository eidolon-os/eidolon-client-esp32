#include "hub_pinned_http.h"

#include "system_info.h"

#include "sdkconfig.h"

#include <esp_http_client.h>
#include <esp_log.h>

#define TAG "HubHttp"

namespace eidolon {

namespace {

// The Hub answers with a descriptor, an enrollment receipt or a handoff
// outcome. All are small JSON documents; the provider binding inside a handoff
// is the largest and stays well under this. A response that exceeds it is not
// something this firmware knows how to consume.
constexpr size_t kMaxResponseBytes = 32 * 1024;

esp_http_client_method_t MethodFor(const std::string& method)
{
    if (method == "POST") {
        return HTTP_METHOD_POST;
    }
    return HTTP_METHOD_GET;
}

}  // namespace

esp_err_t HubHttpRequest(const std::string& method,
                         const std::string& url,
                         const std::string& certificate_pem,
                         const std::string& request_body,
                         HubHttpResponse& out)
{
    out = HubHttpResponse{};
    if (url.rfind("https://", 0) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (certificate_pem.empty()) {
        // Not a transport failure: this device has no reason to believe the
        // machine answering is the Host it belongs to.
        ESP_LOGE(TAG, "Refusing Hub request without a commissioned certificate");
        return ESP_ERR_NOT_ALLOWED;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = MethodFor(method);
    config.timeout_ms = CONFIG_EIDOLON_CONFIG_HTTP_TIMEOUT_MS;
    // esp_http_client requires a NUL-terminated PEM and counts the terminator.
    config.cert_pem = certificate_pem.c_str();
    config.cert_len = certificate_pem.size() + 1;
    config.disable_auto_redirect = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    const std::string user_agent = SystemInfo::GetUserAgent();
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", user_agent.c_str());
    if (!request_body.empty()) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }

    err = esp_http_client_open(client, request_body.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s %s failed to open: %s", method.c_str(), url.c_str(),
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    if (!request_body.empty()) {
        const int written =
            esp_http_client_write(client, request_body.data(), request_body.size());
        if (written != static_cast<int>(request_body.size())) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length > static_cast<int64_t>(kMaxResponseBytes)) {
        ESP_LOGE(TAG, "Hub response of %lld bytes exceeds what this firmware reads",
                 static_cast<long long>(content_length));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    std::string body;
    char chunk[512];
    while (true) {
        const int read = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        if (body.size() + static_cast<size_t>(read) > kMaxResponseBytes) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        body.append(chunk, static_cast<size_t>(read));
        if (esp_http_client_is_complete_data_received(client)) {
            break;
        }
    }

    if (err == ESP_OK) {
        out.status = esp_http_client_get_status_code(client);
        out.body = std::move(body);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

}  // namespace eidolon
