#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_heap_caps.h>

namespace eidolon {

// Activation reads OTA metadata and other flash-backed state. ESP-IDF can
// disable the external-memory cache while doing that work, so the executing
// task's stack must remain in internal RAM for the whole actor lifetime.
inline constexpr std::size_t kActivationWorkerStackBytes = 8 * 1024;
inline constexpr std::uint32_t kActivationWorkerMemoryCaps =
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

static_assert((kActivationWorkerMemoryCaps & MALLOC_CAP_SPIRAM) == 0,
              "activation worker stack must remain cache-independent");

}  // namespace eidolon
