#pragma once
// The view seeds the pet's random life once at build time.
#include <cstdint>
inline uint32_t esp_random() { return 0x5EED1234u; }
