#pragma once
#include <cstdint>
// Deterministic request ids in the host test.
inline uint32_t esp_random() { return 0x12345678u; }
