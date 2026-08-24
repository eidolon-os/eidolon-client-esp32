#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_heap_caps.h>

namespace eidolon {

// Activation reads OTA metadata and other flash-backed state. ESP-IDF can
// disable the external-memory cache while doing that work, so the executing
// task's stack must remain in internal RAM for the whole actor lifetime.
// Canonical Claim creation also performs P-256 signing on this actor. Box3 HIL
// proved that the complete discovery -> descriptor -> Claim -> signing call
// chain exhausts 8 KiB before the signer can return; reserve the measured
// crypto/control-plane budget here instead of hiding the failure with a retry.
inline constexpr std::size_t kActivationWorkerStackBytes = 16 * 1024;
inline constexpr std::uint32_t kActivationWorkerMemoryCaps =
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

static_assert((kActivationWorkerMemoryCaps & MALLOC_CAP_SPIRAM) == 0,
              "activation worker stack must remain cache-independent");

}  // namespace eidolon
