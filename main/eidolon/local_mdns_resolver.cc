#include "sdkconfig.h"
#include <lwip/dns.h>
#include <lwip/ip_addr.h>

#if CONFIG_EIDOLON_HUB_MODE
#include "local_mdns_resolver.h"
#include <mdns.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <strings.h>
#include <mutex>

namespace eidolon {
esp_err_t EnsureLocalMdnsInitialized() {
    static std::mutex mutex;
    static bool initialized = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (initialized) return ESP_OK;
    const esp_err_t err = mdns_init();
    if (err == ESP_OK) initialized = true;
    return err;
}
}

namespace {
esp_err_t Query(const char* host, bool ipv6, ip_addr_t* address) {
#if LWIP_IPV6
    if (ipv6) {
        esp_ip6_addr_t result = {};
        const auto err = mdns_query_aaaa(host, CONFIG_EIDOLON_MDNS_QUERY_TIMEOUT_MS, &result);
        if (err == ESP_OK) {
            ip_addr_set_zero_ip6(address);
            std::memcpy(ip_2_ip6(address)->addr, result.addr, sizeof(result.addr));
        }
        return err;
    }
#else
    if (ipv6) return ESP_ERR_NOT_SUPPORTED;
#endif
#if LWIP_IPV4
    esp_ip4_addr_t result = {};
    const auto err = mdns_query_a(host, CONFIG_EIDOLON_MDNS_QUERY_TIMEOUT_MS, &result);
    if (err == ESP_OK) {
        ip_addr_set_zero_ip4(address);
        ip_2_ip4(address)->addr = result.addr;
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
}
#endif

// ESP-IDF's netconn hook runs in the caller, so bounded blocking mDNS queries
// are safe here. DNS_EXTERNAL_RESOLVE runs on TCPIP and must not be used for this.
// Native mDNS validates the answer's owner name; lwIP's legacy-unicast mDNS
// parser in IDF 5.5 accepts an unrelated A record when the question matches.
extern "C" int lwip_hook_netconn_external_resolve(
        const char* name, ip_addr_t* address, u8_t addrtype, err_t* error) {
#if CONFIG_EIDOLON_HUB_MODE
    if (!name || !address || !error) return 0;
    size_t length = std::strlen(name);
    if (length && name[length - 1] == '.') --length;
    constexpr size_t suffix = sizeof(".local") - 1;
    if (length <= suffix || strncasecmp(name + length - suffix, ".local", suffix)) return 0;

    // One DNS hostname label, as accepted by the native mdns_query_a/aaaa API.
    const size_t host_length = length - suffix;
    char host[64];
    if (host_length >= sizeof(host) || std::memchr(name, '.', host_length)) {
        *error = ERR_ARG;
        return 1;
    }
    std::memcpy(host, name, host_length);
    host[host_length] = '\0';
    const auto started = esp_timer_get_time();
    esp_err_t result = eidolon::EnsureLocalMdnsInitialized();
    if (result == ESP_OK) {
        const bool ipv6 = addrtype == LWIP_DNS_ADDRTYPE_IPV6 ||
                          addrtype == LWIP_DNS_ADDRTYPE_IPV6_IPV4;
        result = Query(host, ipv6, address);
        if (result == ESP_ERR_NOT_FOUND &&
            (addrtype == LWIP_DNS_ADDRTYPE_IPV4_IPV6 ||
             addrtype == LWIP_DNS_ADDRTYPE_IPV6_IPV4)) {
            result = Query(host, !ipv6, address);
        }
    }
    *error = result == ESP_OK ? ERR_OK : result == ESP_ERR_NO_MEM ? ERR_MEM : ERR_TIMEOUT;
    char resolved[IPADDR_STRLEN_MAX] = "-";
    if (result == ESP_OK) ipaddr_ntoa_r(address, resolved, sizeof(resolved));
    ESP_LOGI("LocalMdns", "resolve name=%s family=%u result=%s address=%s ms=%lu", name,
             static_cast<unsigned>(addrtype), esp_err_to_name(result),
             resolved,
             static_cast<unsigned long>((esp_timer_get_time() - started) / 1000));
    // A failed .local lookup must not fall through to the legacy parser/cache.
    return 1;
#else
    (void)name; (void)address; (void)addrtype; (void)error;
    return 0;
#endif
}
