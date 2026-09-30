#include "eidolon/local_mdns_resolver.h"
#include <lwip/dns.h>
#include <mdns.h>
#include <atomic>
#include <cassert>
#include <string>
#include <thread>
#include <vector>
extern "C" int lwip_hook_netconn_external_resolve(const char*, ip_addr_t*, u8_t, err_t*);
std::atomic<int> init_calls{0};
esp_err_t init_result = ESP_ERR_NO_MEM, a_result = ESP_OK, aaaa_result = ESP_OK;
std::string families, requested;
const char* esp_err_to_name(esp_err_t) { return "test"; }
esp_err_t mdns_init() { ++init_calls; return init_result; }
esp_err_t mdns_query_a(const char* host, uint32_t timeout, esp_ip4_addr_t* out) {
    assert(timeout == 8000); requested = host; families += '4'; out->addr = 230; return a_result;
}
esp_err_t mdns_query_aaaa(const char* host, uint32_t timeout, esp_ip6_addr_t* out) {
    assert(timeout == 8000); requested = host; families += '6'; out->addr[3] = 230; return aaaa_result;
}
int main() {
    ip_addr_t address; err_t error = 99;
    for (const char* name : {"192.168.3.230", "::1", "hub.example.com", "hub.local.example", "local", ""}) {
        assert(lwip_hook_netconn_external_resolve(name, &address, 0, &error) == 0);
        assert(error == 99);
    }
    assert(init_calls == 0);
    assert(lwip_hook_netconn_external_resolve("hub.local", &address, 0, &error) == 1);
    assert(error == ERR_MEM && init_calls == 1 && families.empty());
    init_result = ESP_OK;
    std::vector<std::thread> workers;
    for (int i=0;i<16;++i) workers.emplace_back([]{ assert(eidolon::EnsureLocalMdnsInitialized() == ESP_OK); });
    for (auto& worker : workers) worker.join();
    assert(init_calls == 2);
    assert(lwip_hook_netconn_external_resolve("Hub.LOCAL.", &address, 0, &error) == 1);
    assert(error == ERR_OK && requested == "Hub" && families == "4" && address.v4.addr == 230);
    families.clear(); a_result = ESP_ERR_NOT_FOUND;
    assert(lwip_hook_netconn_external_resolve("hub.local", &address, 0, &error) == 1);
    assert(error == ERR_TIMEOUT && families == "4"); // handled failure: no legacy resolver fallback
#if LWIP_IPV6
    families.clear();
    assert(lwip_hook_netconn_external_resolve("hub.local", &address, 2, &error) == 1);
    assert(error == ERR_OK && families == "46" && address.type == 6 && address.v6.addr[3] == 230);
    families.clear(); a_result = ESP_OK; aaaa_result = ESP_ERR_NOT_FOUND;
    assert(lwip_hook_netconn_external_resolve("hub.local", &address, 3, &error) == 1);
    assert(error == ERR_OK && families == "64" && address.type == 4);
    families.clear();
    assert(lwip_hook_netconn_external_resolve("hub.local", &address, 1, &error) == 1);
    assert(error == ERR_TIMEOUT && families == "6");
#endif
    families.clear(); a_result = ESP_ERR_NO_MEM;
    assert(lwip_hook_netconn_external_resolve("hub.local", &address, 2, &error) == 1);
    assert(error == ERR_MEM && families == "4"); // do not disguise memory failures as another-family lookup
    families.clear();
    for (const auto& name : {std::string(64, 'x') + ".local", std::string("a.b.local")}) {
        assert(lwip_hook_netconn_external_resolve(name.c_str(), &address, 0, &error) == 1);
        assert(error == ERR_ARG && families.empty());
    }
}
