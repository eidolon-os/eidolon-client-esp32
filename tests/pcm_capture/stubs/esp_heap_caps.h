#pragma once
#include <cstddef>
constexpr unsigned MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_SPIRAM=4;
inline size_t heap_caps_get_free_size(unsigned) { return 100000; }
