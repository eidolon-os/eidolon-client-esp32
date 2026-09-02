#include "session_memory_admission_core.h"

// On the device this picks up the LiveKit component's real Kconfig values, so a
// change to CONFIG_LK_ENGINE_QUEUE_SIZE or CONFIG_LK_ENGINE_TASK_STACK_SIZE
// moves the admission rule with it instead of leaving a stale model behind. On
// the host there is no sdkconfig, and the fallbacks below are the SDK's own
// defaults, which is what the firmware ships with.
#if defined(__has_include)
#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif
#endif

namespace eidolon {

namespace {

// --- The engine's internal-RAM requirement, derived from the pinned SDK -------
//
// These numbers come from managed_components/livekit__livekit (our fork, pinned
// in main/idf_component.yml). They are compiled in here rather than read from
// the SDK because engine.c's types are private to that component — so if the
// pin moves, re-check them against core/engine.c's engine_init() and the note in
// docs/livekit-sdk-fork.md.
//
// Every one of these is a FreeRTOS object, and every FreeRTOS object comes from
// pvPortMalloc(), which ESP-IDF hardcodes to MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
// (components/freertos/heap_idf.c). None of it can land in PSRAM, however much
// PSRAM the board has.

#ifdef CONFIG_LK_ENGINE_QUEUE_SIZE
constexpr std::size_t kEngineEventQueueSlots = CONFIG_LK_ENGINE_QUEUE_SIZE;
#else
// The SDK Kconfig default, which is also what this firmware's sdkconfig holds.
constexpr std::size_t kEngineEventQueueSlots = 32;
#endif

// sizeof(engine_event_t) from core/engine.c: a 4-byte type tag, then an
// 8-byte-aligned union whose largest member is livekit_pb_signal_response_t
// (288 bytes, measured from protocol/livekit_rtc.pb.h) — so 8 + 288.
constexpr std::size_t kEngineEventBytes = 296;

// ESP-IDF's xTaskCreate() takes the stack depth in BYTES, unlike vanilla
// FreeRTOS, so this number needs no conversion.
#ifdef CONFIG_LK_ENGINE_TASK_STACK_SIZE
constexpr std::size_t kEngineTaskStackBytes = CONFIG_LK_ENGINE_TASK_STACK_SIZE;
#else
constexpr std::size_t kEngineTaskStackBytes = 8192;
#endif

// The small internal objects engine_init() and signal_init() create around the
// two big ones: the engine task's TCB, its binary done-semaphore, the engine
// timer, the signaling client's ping-interval and ping-timeout timers, and the
// websocket client's recursive mutex and event loop. Rounded DOWN — a floor that
// under-reports is safe here, because over-reporting would refuse attempts that
// would have succeeded. The websocket client's two 20 KiB signalling buffers are
// plain malloc() and land in PSRAM under CONFIG_SPIRAM_USE_MALLOC, so they are
// deliberately absent.
constexpr std::size_t kEngineIncidentalBytes = 2048;

}  // namespace

EngineInternalMemoryRequirement LiveKitEngineInternalMemoryRequirement()
{
    EngineInternalMemoryRequirement requirement;
    requirement.event_queue_bytes = kEngineEventQueueSlots * kEngineEventBytes;
    requirement.task_stack_bytes = kEngineTaskStackBytes;
    requirement.incidental_bytes = kEngineIncidentalBytes;
    return requirement;
}

const char* SessionMemoryVerdictName(SessionMemoryVerdict verdict)
{
    switch (verdict) {
    case SessionMemoryVerdict::Sufficient:
        return "sufficient";
    case SessionMemoryVerdict::Fragmented:
        return "fragmented";
    case SessionMemoryVerdict::Exhausted:
        return "exhausted";
    }
    return "unknown";
}

std::size_t LargestContiguousRequirement(const EngineInternalMemoryRequirement& requirement)
{
    return requirement.event_queue_bytes > requirement.task_stack_bytes
               ? requirement.event_queue_bytes
               : requirement.task_stack_bytes;
}

std::size_t TotalRequirement(const EngineInternalMemoryRequirement& requirement)
{
    return requirement.event_queue_bytes + requirement.task_stack_bytes +
           requirement.incidental_bytes;
}

std::size_t ContiguousShortfallBytes(const InternalHeapSnapshot& heap,
                                     const EngineInternalMemoryRequirement& requirement)
{
    const std::size_t needed = LargestContiguousRequirement(requirement);
    return heap.largest_free_block >= needed ? 0 : needed - heap.largest_free_block;
}

std::size_t TotalShortfallBytes(const InternalHeapSnapshot& heap,
                                const EngineInternalMemoryRequirement& requirement)
{
    const std::size_t needed = TotalRequirement(requirement);
    return heap.free_bytes >= needed ? 0 : needed - heap.free_bytes;
}

SessionMemoryVerdict JudgeSessionMemory(const InternalHeapSnapshot& heap,
                                        const EngineInternalMemoryRequirement& requirement)
{
    // Total first: if the sum is short, no amount of coalescing helps, and
    // saying "fragmented" would send a reader looking for the wrong culprit.
    if (TotalShortfallBytes(heap, requirement) > 0) {
        return SessionMemoryVerdict::Exhausted;
    }
    if (ContiguousShortfallBytes(heap, requirement) > 0) {
        return SessionMemoryVerdict::Fragmented;
    }
    return SessionMemoryVerdict::Sufficient;
}

bool SessionMemoryRetryLedger::RecordRefusal(
    const InternalHeapSnapshot& heap, const EngineInternalMemoryRequirement& requirement)
{
    ++consecutive_refusals_;
    last_verdict_ = JudgeSessionMemory(heap, requirement);
    last_contiguous_shortfall_ = ContiguousShortfallBytes(heap, requirement);
    // Name the number that decided this refusal, not the one that happens to be
    // easiest to reach. JudgeSessionMemory tests the total first, so an
    // Exhausted verdict says nothing about contiguity and its contiguous
    // shortfall is routinely zero.
    last_binding_shortfall_ = last_verdict_ == SessionMemoryVerdict::Exhausted
                                  ? TotalShortfallBytes(heap, requirement)
                                  : last_contiguous_shortfall_;

    if (last_verdict_ == SessionMemoryVerdict::Exhausted) {
        // Nothing to wait for: the working set is larger than the internal heap.
        ceiling_reached_ = true;
        return false;
    }

    const bool progressed =
        heap.largest_free_block >= best_largest_free_block_ + kProgressMarginBytes;
    if (progressed) {
        attempts_since_progress_ = 0;
    } else {
        ++attempts_since_progress_;
    }
    if (heap.largest_free_block > best_largest_free_block_) {
        best_largest_free_block_ = heap.largest_free_block;
    }

    if (attempts_since_progress_ >= kStalledAttemptLimit) {
        ceiling_reached_ = true;
        return false;
    }
    return true;
}

void SessionMemoryRetryLedger::Reset()
{
    ceiling_reached_ = false;
    consecutive_refusals_ = 0;
    attempts_since_progress_ = 0;
    best_largest_free_block_ = 0;
    last_contiguous_shortfall_ = 0;
    last_binding_shortfall_ = 0;
    last_verdict_ = SessionMemoryVerdict::Sufficient;
}

}  // namespace eidolon
