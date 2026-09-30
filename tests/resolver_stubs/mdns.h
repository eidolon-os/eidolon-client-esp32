#pragma once
#include <esp_err.h>
#include <cstdint>
struct esp_ip4_addr_t { uint32_t addr; };
struct esp_ip6_addr_t { uint32_t addr[4]; };
esp_err_t mdns_init();
esp_err_t mdns_query_a(const char*, uint32_t, esp_ip4_addr_t*);
esp_err_t mdns_query_aaaa(const char*, uint32_t, esp_ip6_addr_t*);
