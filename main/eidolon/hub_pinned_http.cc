#include "hub_pinned_http.h"

#include "host_resolution_core.h"
#include "http_date_utc.h"

#include "system_info.h"

#include "sdkconfig.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <lwip/dns.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
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

// Where a URL's host actually resolves to, for the log line that reports a
// failure to reach it.
//
// "Failed to connect" without an address cannot be acted on: a Host that is up
// and a name that resolves somewhere else produce the identical message, and
// only the second one is the device looking at the wrong machine. Resolution
// here uses the same family this transport binds to, so what is reported is
// what was attempted.
std::string ResolvedAddressOf(const std::string& url)
{
    const std::string host = HostOfUrl(url);
    if (host.empty()) {
        return "unparsable-url";
    }
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || results == nullptr) {
        return host + " does not resolve";
    }
    std::string reported = host + " does not resolve";
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || entry->ai_addr == nullptr) {
            continue;
        }
        char text[INET_ADDRSTRLEN] = {};
        const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr) {
            reported = host + " -> " + text;
            break;
        }
    }
    freeaddrinfo(results);
    return reported;
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
    ESP_LOGW(TAG, "Dropping the cached address for %s so the next attempt resolves again",
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
        ESP_LOGE(TAG, "%s %s failed to open: %s (%s)", method.c_str(), url.c_str(),
                 esp_err_to_name(err), ResolvedAddressOf(url).c_str());
        esp_http_client_cleanup(client);
        DropCachedResolutionIfStale(HostOfUrl(url),
                                    HubRequestFailure::ConnectionNotOpened);
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
