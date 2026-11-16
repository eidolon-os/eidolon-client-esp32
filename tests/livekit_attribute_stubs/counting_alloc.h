// nanopb's allocator, counted, so a test can see every decoded field freed.
#pragma once
#include <stddef.h>
void* counting_realloc(void* ptr, size_t size);
void counting_free(void* ptr);
#define pb_realloc(ptr, size) counting_realloc(ptr, size)
#define pb_free(ptr) counting_free(ptr)
