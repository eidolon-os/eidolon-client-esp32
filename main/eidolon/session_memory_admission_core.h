#ifndef EIDOLON_SESSION_MEMORY_ADMISSION_CORE_H_
#define EIDOLON_SESSION_MEMORY_ADMISSION_CORE_H_

#include <cstddef>

namespace eidolon {

// Whether this device can build a LiveKit room right now, decided from the
// internal heap rather than discovered by watching livekit_room_create() return
// NULL with the words "Failed to create engine".
//
// Why this exists as its own core: the field failure on the Waveshare 2.06
// AMOLED board reported 22299 bytes of free internal RAM and still could not
// build an engine, because what the engine needs is not 22 KB of internal RAM —
// it is two SEPARATE contiguous blocks, the larger of them ~9.3 KiB, out of a
// heap whose largest hole was 7680 bytes. Every allocation the FreeRTOS API
// makes goes through pvPortMalloc(), which is hardcoded to
// MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, so the engine's event queue and task
// stack cannot fall back to the 5.8 MB of PSRAM sitting idle on the board.
//
// Judging this before the call, from named numbers, is what turns an opaque SDK
// error into a line that says how many bytes are missing and whether waiting
// will ever produce them.

// The internal-RAM blocks livekit_room_create() -> engine_init() must obtain.
// Kept as separate fields on purpose: a heap can hold the total and still fail,
// which is exactly what happened in the field.
struct EngineInternalMemoryRequirement {
    // xQueueCreate(CONFIG_LK_ENGINE_QUEUE_SIZE, sizeof(engine_event_t)) — one
    // contiguous block.
    std::size_t event_queue_bytes = 0;
    // xTaskCreate(engine_task, ..., CONFIG_LK_ENGINE_TASK_STACK_SIZE, ...) —
    // one contiguous block. ESP-IDF's stack depth argument is bytes, not words.
    std::size_t task_stack_bytes = 0;
    // The small internal objects around them: the engine task's TCB, its done
    // semaphore, the engine timer, the signaling client's two ping timers and
    // its mutex/event loop. Individually harmless, collectively not.
    std::size_t incidental_bytes = 0;
};

// The requirement of the LiveKit SDK fork this firmware is pinned to. Derived
// from the SDK's own Kconfig defaults and structure layout, and deliberately a
// LOWER bound: admission must never refuse an attempt that could have worked, so
// every rounding here rounds down. See the .cc for each number's derivation.
EngineInternalMemoryRequirement LiveKitEngineInternalMemoryRequirement();

// What the internal heap looks like at one instant. Mirrors what
// heap_caps_get_info(MALLOC_CAP_INTERNAL) reports; free_blocks is carried
// because it is the difference between "shattered" and "full".
struct InternalHeapSnapshot {
    std::size_t free_bytes = 0;
    std::size_t largest_free_block = 0;
    std::size_t min_free_bytes = 0;
    std::size_t free_blocks = 0;
};

enum class SessionMemoryVerdict {
    // Both the contiguous and the total requirement fit. Go.
    Sufficient,
    // The total fits but no single hole is big enough. Coalescing could still
    // produce one, so another attempt is not yet pointless.
    Fragmented,
    // The total itself is short. No arrangement of the free space satisfies the
    // engine; only a smaller working set will.
    Exhausted,
};

const char* SessionMemoryVerdictName(SessionMemoryVerdict verdict);

// The largest single block the engine will ask for. It is the MAX of the two big
// allocations, not their sum: they are separate calls, so the second one can
// reuse the space the first did not need.
std::size_t LargestContiguousRequirement(const EngineInternalMemoryRequirement& requirement);
std::size_t TotalRequirement(const EngineInternalMemoryRequirement& requirement);

SessionMemoryVerdict JudgeSessionMemory(const InternalHeapSnapshot& heap,
                                        const EngineInternalMemoryRequirement& requirement);

// How many bytes are missing, so the failure can say it instead of leaving a
// reader to subtract two numbers from two different log lines.
std::size_t ContiguousShortfallBytes(const InternalHeapSnapshot& heap,
                                     const EngineInternalMemoryRequirement& requirement);
std::size_t TotalShortfallBytes(const InternalHeapSnapshot& heap,
                                const EngineInternalMemoryRequirement& requirement);

// Decides whether a memory refusal is worth retrying, which is the part the
// device got wrong in the field: five attempts, 30 s apart, against a ceiling
// that never moved, with nothing in the log saying it never would.
//
// A refusal is worth retrying only while the heap is still moving in the right
// direction. "Progress" means the largest free block beat the best ever seen by
// a margin bigger than allocator noise; three attempts without progress means
// the shortfall is a standing condition and the device should say so once rather
// than keep asking. Exhaustion is terminal on sight.
class SessionMemoryRetryLedger {
public:
    // Bytes of improvement below which a bigger largest-block is noise from
    // other tasks' churn, not the heap actually recovering.
    static constexpr std::size_t kProgressMarginBytes = 512;
    // Attempts allowed to make no progress before the ceiling is called.
    static constexpr int kStalledAttemptLimit = 3;

    // Records one refused attempt and returns whether another is worth making.
    bool RecordRefusal(const InternalHeapSnapshot& heap,
                       const EngineInternalMemoryRequirement& requirement);

    // Ends the current run of refusals, because something changed that makes the
    // measurements in it no longer describe the device: a room that did get
    // built, or a fresh edge (network restored, re-activation, a person asking
    // for a conversation) that legitimately earns one more look. The next
    // refusal is then a new fact about a new moment rather than attempt six of
    // the old one.
    void Reset();

    bool ceiling_reached() const { return ceiling_reached_; }
    // The shortfall that actually decided the refusal: the total for an
    // exhausted heap, the largest contiguous block for a fragmented one. These
    // are different numbers and reporting the wrong one is not a cosmetic slip
    // — an exhausted heap has no contiguous shortfall by construction, so a
    // caller printing last_contiguous_shortfall() tells the reader a device
    // that is genuinely short of RAM is "short by 0 bytes". That line sent the
    // first reader of this failure looking for fragmentation that was never
    // there.
    std::size_t last_binding_shortfall() const { return last_binding_shortfall_; }
    bool retry_worthwhile() const { return !ceiling_reached_; }
    int consecutive_refusals() const { return consecutive_refusals_; }
    int attempts_since_progress() const { return attempts_since_progress_; }
    std::size_t best_largest_free_block() const { return best_largest_free_block_; }
    std::size_t last_contiguous_shortfall() const { return last_contiguous_shortfall_; }
    SessionMemoryVerdict last_verdict() const { return last_verdict_; }

private:
    bool ceiling_reached_ = false;
    int consecutive_refusals_ = 0;
    int attempts_since_progress_ = 0;
    std::size_t best_largest_free_block_ = 0;
    std::size_t last_contiguous_shortfall_ = 0;
    std::size_t last_binding_shortfall_ = 0;
    SessionMemoryVerdict last_verdict_ = SessionMemoryVerdict::Sufficient;
};

}  // namespace eidolon

#endif  // EIDOLON_SESSION_MEMORY_ADMISSION_CORE_H_
