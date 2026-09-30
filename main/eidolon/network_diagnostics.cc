#include <cerrno>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

// Optional linker instrumentation: observe the actual resolver/socket calls,
// never repeat a lookup or change DNS, timeout, TLS, or routing behavior.
extern "C" int __real_lwip_getaddrinfo(const char*, const char*,
                                      const struct addrinfo*, struct addrinfo**);
extern "C" int __real_lwip_connect(int, const struct sockaddr*, socklen_t);

namespace {
void Address(const struct sockaddr* address, char* text, size_t size, unsigned& port) {
    text[0] = '\0';
    port = 0;
    if (!address) return;
    if (address->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const struct sockaddr_in*>(address);
        inet_ntop(AF_INET, &v4->sin_addr, text, size);
        port = ntohs(v4->sin_port);
    }
#if LWIP_IPV6
    else if (address->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const struct sockaddr_in6*>(address);
        inet_ntop(AF_INET6, &v6->sin6_addr, text, size);
        port = ntohs(v6->sin6_port);
    }
#endif
}
}

extern "C" int __wrap_lwip_getaddrinfo(const char* host, const char* service,
                                      const struct addrinfo* hints, struct addrinfo** out) {
    const int before = errno;
    const auto start = esp_timer_get_time();
    ESP_LOGI("NetTrace", "dns begin task=%s host=%s", pcTaskGetName(nullptr), host ? host : "");
    errno = before;
    const int result = __real_lwip_getaddrinfo(host, service, hints, out);
    const int saved = errno;
    char address[48] = {};
    unsigned port = 0;
    if (!result && out && *out) Address((*out)->ai_addr, address, sizeof(address), port);
    ESP_LOGI("NetTrace", "dns end task=%s host=%s rc=%d ms=%ld first=%s",
             pcTaskGetName(nullptr), host ? host : "", result,
             static_cast<long>((esp_timer_get_time() - start) / 1000), address);
    errno = saved;
    return result;
}

extern "C" int __wrap_lwip_connect(int socket, const struct sockaddr* target, socklen_t length) {
    const int before = errno;
    char address[48] = {};
    unsigned port = 0;
    Address(target, address, sizeof(address), port);
    const auto start = esp_timer_get_time();
    ESP_LOGI("NetTrace", "connect begin task=%s fd=%d peer=%s:%u",
             pcTaskGetName(nullptr), socket, address, port);
    errno = before;
    const int result = __real_lwip_connect(socket, target, length);
    const int saved = errno;
    ESP_LOGI("NetTrace", "connect end fd=%d rc=%d errno=%d ms=%ld", socket, result,
             result < 0 ? saved : 0, static_cast<long>((esp_timer_get_time() - start) / 1000));
    errno = saved;
    return result;
}
