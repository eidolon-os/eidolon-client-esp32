#pragma once
#include <cstdint>
#include <cstring>
using u8_t = uint8_t;
using err_t = int;
constexpr int ERR_OK = 0, ERR_MEM = -1, ERR_TIMEOUT = -3, ERR_ARG = -16;
struct ip4_addr_t { uint32_t addr; };
struct ip6_addr_t { uint32_t addr[4]; };
struct ip_addr_t { ip4_addr_t v4{}; ip6_addr_t v6{}; int type = 0; };
#define ip_2_ip4(p) (&(p)->v4)
#define ip_2_ip6(p) (&(p)->v6)
#define ip_addr_set_zero_ip4(p) (*(p) = ip_addr_t{}, (p)->type = 4)
#define ip_addr_set_zero_ip6(p) (*(p) = ip_addr_t{}, (p)->type = 6)
#define IPADDR_STRLEN_MAX 46
inline char* ipaddr_ntoa_r(const ip_addr_t*, char* out, int) { return std::strcpy(out, "test-address"); }
