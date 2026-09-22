#include "hub_pinned_http.h"

#include "host_resolution_core.h"
#include "http_date_utc.h"

#include "system_info.h"

#include "sdkconfig.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>

#include <strings.h>

#include <cstdint>

#define TAG "HubHttp"

namespace eidolon {

namespace {

// The Hub answers with a descriptor, an enrollment receipt or a handoff
// outcome. All are small JSON documents; the provider binding inside a handoff
// is the largest and stays well under this. A response that exceeds it is not
// something this firmware knows how to consume.
constexpr size_t kMaxResponseBytes = 32 * 1024;

// Records what the Hub said the time was, for the caller that has to judge one
// of the Hub's own deadlines. Nothing else in this response can supply it: the
// header is gone by the time the body is read, and this device's own wall clock
// is never set in this build.
esp_err_t CaptureHubDate(esp_http_client_event_t* event)
{
    if (event == nullptr || event->event_id != HTTP_EVENT_ON_HEADER ||
        event->user_data == nullptr || event->header_key == nullptr ||
        event->header_value == nullptr ||
        strcasecmp(event->header_key, "Date") != 0) {
        return ESP_OK;
    }
    int64_t stated = 0;
    if (!ParseHttpDateUtcMillis(event->header_value, stated)) {
        ESP_LOGW(TAG, "Hub stated a Date this device cannot read: %s",
                 event->header_value);
        return ESP_OK;
    }
    *static_cast<int64_t*>(event->user_data) = stated;
    return ESP_OK;
}

esp_http_client_method_t MethodFor(const std::string& method)
{
    if (method == "POST") {
        return HTTP_METHOD_POST;
    }
    return HTTP_METHOD_GET;
}

// Read transport evidence before close/cleanup destroys it. Do not resolve the
// name again here: a later lookup is not evidence of the attempted address.
// The open phase includes DNS, TCP and TLS; the SDK error distinguishes them
// when available. Zero TLS evidence must not be interpreted as a successful TLS
// handshake (the failure may have occurred before one was attempted).
void LogRequestFailure(esp_http_client_handle_t client,
                       const std::string& method, const std::string& url,
                       const char* phase, esp_err_t error, int raw_result,
                       int64_t started_ms)
{
    const int socket_error = client ? esp_http_client_get_errno(client) : 0;
    int tls_code = 0;
    int tls_flags = 0;
    const esp_err_t tls_error = client
        ? esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags)
        : ESP_OK;
    const int status = client ? esp_http_client_get_status_code(client) : 0;
    // Use 32-bit formats supported by the production newlib nano printf.
    ESP_LOGE(TAG,
             "%s %s phase=%s error=%s raw=%d elapsed_ms=%lu status=%d "
             "socket_errno=%d tls_error=0x%x tls_code=%d tls_flags=0x%x",
             method.c_str(), url.c_str(), phase, esp_err_to_name(error), raw_result,
             static_cast<unsigned long>(esp_timer_get_time() / 1000 - started_ms),
             status, socket_error, static_cast<unsigned int>(tls_error), tls_code,
             static_cast<unsigned int>(tls_flags));
}

// dns_table is TCPIP-thread state and this build has no core locking
// (CONFIG_LWIP_TCPIP_CORE_LOCKING is off, so LOCK_TCPIP_CORE is a no-op), so
// the clear is marshalled onto that thread rather than raced from this one.
// Waiting for it matters: the next attempt has to resolve again, not race the
// clear that was supposed to precede it.
//
// dns_clear_cache() drops every entry, not one name. On a device that talks to
// one Host that is a handful of names, and it fails any lookup already in
// flight, whose caller retries.
void ClearDnsCacheOnTcpipThread(void*)
{
    dns_clear_cache();
}

void DropCachedResolutionIfStale(const std::string& host, HubRequestFailure failure)
{
    if (!ShouldDropCachedResolution(host, failure)) {
        return;
    }
    ESP_LOGW(TAG, "Clearing DNS cache after connection failure for %s",
             host.c_str());
    // Never called from the TCPIP thread itself, which would deadlock here.
    tcpip_callback_wait(ClearDnsCacheOnTcpipThread, nullptr);
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
    // The current Host LAN ingress is an IPv4 transport adapter and App-ready
    // proves its IPv4 address/listener. AF_UNSPEC can retain or select an mDNS
    // IPv6 answer that the ingress does not serve, preventing retry recovery
    // until the device is rebooted. Keep the logical URI/Host/SNI unchanged and
    // bind only this transport adapter to the address family it actually serves.
    config.addr_type = HTTP_ADDR_TYPE_INET;
    // Kept across the whole call: the handler fires while headers are parsed,
    // and `out` outlives every one of those.
    config.event_handler = CaptureHubDate;
    config.user_data = &out.hub_utc_millis;

    const int64_t diagnostic_start_ms = esp_timer_get_time() / 1000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        LogRequestFailure(nullptr, method, url, "init", ESP_ERR_NO_MEM, 0, diagnostic_start_ms);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    const std::string user_agent = SystemInfo::GetUserAgent();
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", user_agent.c_str());
    if (!request_body.empty()) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }

    const int64_t request_start_ms = esp_timer_get_time() / 1000;
    err = esp_http_client_open(client, request_body.size());
    if (err != ESP_OK) {
        LogRequestFailure(client, method, url, "open", err, err, diagnostic_start_ms);
        esp_http_client_cleanup(client);
        DropCachedResolutionIfStale(HostOfUrl(url),
                                    HubRequestFailure::ConnectionNotOpened);
        return err;
    }

    if (!request_body.empty()) {
        const int written =
            esp_http_client_write(client, request_body.data(), request_body.size());
        if (written != static_cast<int>(request_body.size())) {
            LogRequestFailure(client, method, url, "write-body", ESP_FAIL, written,
                              diagnostic_start_ms);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        LogRequestFailure(client, method, url, "read-headers", ESP_FAIL,
                          static_cast<int>(content_length), diagnostic_start_ms);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length > static_cast<int64_t>(kMaxResponseBytes)) {
        LogRequestFailure(client, method, url, "response-size", ESP_ERR_INVALID_SIZE,
                          0, diagnostic_start_ms);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    std::string body;
    char chunk[512];
    while (true) {
        const int read = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read < 0) {
            LogRequestFailure(client, method, url, "read-body", ESP_FAIL, read,
                              diagnostic_start_ms);
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            if (!esp_http_client_is_complete_data_received(client)) {
                LogRequestFailure(client, method, url, "incomplete-body",
                                  ESP_ERR_HTTP_INCOMPLETE_DATA, read, diagnostic_start_ms);
            }
            break;
        }
        if (body.size() + static_cast<size_t>(read) > kMaxResponseBytes) {
            LogRequestFailure(client, method, url, "response-size", ESP_ERR_INVALID_SIZE,
                              read, diagnostic_start_ms);
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
        out.clock.Observe(out.hub_utc_millis, request_start_ms, esp_timer_get_time() / 1000);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

}  // namespace eidolon
