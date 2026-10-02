#include <cerrno>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <esp_peer.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <lwip/ip4.h>
#include <lwip/inet_chksum.h>

// Optional linker instrumentation: observe the actual resolver/socket calls,
// never repeat a lookup or change DNS, timeout, TLS, or routing behavior.
extern "C" int __real_lwip_getaddrinfo(const char*, const char*,
                                      const struct addrinfo*, struct addrinfo**);
extern "C" int __real_lwip_connect(int, const struct sockaddr*, socklen_t);
extern "C" int __real_esp_peer_send_msg(esp_peer_handle_t, esp_peer_msg_t*);
extern "C" err_t __real_ip4_input(struct pbuf*, struct netif*);
extern "C" u16_t __real_inet_chksum_pseudo(struct pbuf*, u8_t, u16_t,
                                           const ip4_addr_t*, const ip4_addr_t*);

namespace {
// Count arrivals before lwIP validation. No logging, allocation, packet mutation
// or additional checksum calculation runs on the TCPIP receive path.
std::atomic<unsigned> rx_ip4{0}, rx_synack{0}, rx_signaling{0}, rx_mdns{0},
    rx_echo{0}, rx_bad_tcp_checksum{0};
std::atomic<const pbuf*> receiving{nullptr};

void ReceiveSnapshot(void*) {
    ESP_LOGI("NetTrace", "rx ip4=%u synack=%u signaling=%u mdns_answer=%u "
             "icmp_echo=%u bad_tcp_checksum=%u",
             rx_ip4.load(std::memory_order_relaxed), rx_synack.load(std::memory_order_relaxed),
             rx_signaling.load(std::memory_order_relaxed), rx_mdns.load(std::memory_order_relaxed),
             rx_echo.load(std::memory_order_relaxed), rx_bad_tcp_checksum.load(std::memory_order_relaxed));
}

void StartReceiveSnapshots() {
    static esp_timer_handle_t timer = nullptr;
    if (timer) return;
    const esp_timer_create_args_t args = {.callback=ReceiveSnapshot, .name="net_rx_trace"};
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, 5000000);
    }
}

unsigned Be16(const unsigned char* bytes) {
    return (static_cast<unsigned>(bytes[0]) << 8) | bytes[1];
}

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

extern "C" err_t __wrap_ip4_input(struct pbuf* packet, struct netif* input) {
    rx_ip4.fetch_add(1, std::memory_order_relaxed);
    unsigned char header[80] = {};
    const size_t length = pbuf_copy_partial(packet, header, sizeof(header), 0);
    const size_t ip_length = (header[0] & 15) * 4;
    if (length >= 20 && (header[0] >> 4) == 4 && ip_length >= 20 &&
        length >= ip_length && !(Be16(header + 6) & 0x1fff)) {
        const auto* transport = header + ip_length;
        const size_t available = length - ip_length;
        if (header[9] == 6 && available >= 20) {
            if ((transport[13] & 0x12) == 0x12) rx_synack.fetch_add(1, std::memory_order_relaxed);
            if (Be16(transport) == 7880) rx_signaling.fetch_add(1, std::memory_order_relaxed);
        } else if (header[9] == 17 && available >= 20 &&
                   Be16(transport) == 5353 && (transport[10] & 0x80)) {
            rx_mdns.fetch_add(1, std::memory_order_relaxed);
        } else if (header[9] == 1 && available >= 8 && transport[0] == 8) {
            rx_echo.fetch_add(1, std::memory_order_relaxed);
        }
    }
    receiving.store(packet, std::memory_order_relaxed);
    const err_t result = __real_ip4_input(packet, input);
    receiving.store(nullptr, std::memory_order_relaxed);
    return result;
}

extern "C" u16_t __wrap_inet_chksum_pseudo(struct pbuf* packet, u8_t protocol,
                                            u16_t length, const ip4_addr_t* source,
                                            const ip4_addr_t* destination) {
    const u16_t result = __real_inet_chksum_pseudo(packet, protocol, length, source, destination);
    if (protocol == 6 && result && receiving.load(std::memory_order_relaxed) == packet) {
        rx_bad_tcp_checksum.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

extern "C" int __wrap_lwip_getaddrinfo(const char* host, const char* service,
                                      const struct addrinfo* hints, struct addrinfo** out) {
    const int before = errno;
    // DNS callers can run concurrently. Creation is serialized outside TCPIP.
    static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
    static bool started = false;
    bool start_snapshots = false;
    portENTER_CRITICAL(&init_mux);
    if (!started) { started = true; start_snapshots = true; }
    portEXIT_CRITICAL(&init_mux);
    if (start_snapshots) StartReceiveSnapshots();
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

extern "C" int __wrap_esp_peer_send_msg(esp_peer_handle_t peer, esp_peer_msg_t* message) {
    const int before = errno;
    char transport[8] = "-";
    if (message && message->type == ESP_PEER_MSG_TYPE_CANDIDATE &&
        message->data && message->size > 0) {
        char prefix[96] = {};
        const size_t count = static_cast<size_t>(message->size) < sizeof(prefix) - 1
            ? static_cast<size_t>(message->size) : sizeof(prefix) - 1;
        std::memcpy(prefix, message->data, count);
        std::sscanf(prefix, "%*s %*u %7s", transport);
    }
    ESP_LOGI("NetTrace", "rtc input peer=%p type=%d transport=%s bytes=%d", peer,
             message ? static_cast<int>(message->type) : -1, transport,
             message ? message->size : 0);
    errno = before;
    const int result = __real_esp_peer_send_msg(peer, message);
    const int saved = errno;
    ESP_LOGI("NetTrace", "rtc result peer=%p rc=%d", peer, result);
    errno = saved;
    return result;
}
