#pragma once
#include <cstddef>
#include <cstdint>
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
inline size_t heap_caps_get_free_size(uint32_t) { return 0; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return 0; }
inline bool heap_caps_check_integrity_all(bool) { return true; }
