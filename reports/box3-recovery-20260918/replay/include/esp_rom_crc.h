#pragma once
#include <cstdint>
#include <zlib.h>
inline uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t* p,uint32_t n) { return crc32(crc,p,n); }
