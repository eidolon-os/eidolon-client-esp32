#ifndef EIDOLON_INTERNAL_MEMORY_REPORT_H_
#define EIDOLON_INTERNAL_MEMORY_REPORT_H_

#include "session_memory_admission_core.h"

namespace eidolon {

// Internal-RAM accounting, so "who is holding the internal heap" is a question
// the serial log answers instead of one a person reconstructs from guesses.
//
// The failure this exists for reported one number — 22299 bytes free — and that
// number was not the reason for anything. The reasons were that the free space
// was in nine pieces, that the largest was 7680 bytes, and that a handful of
// tasks were sitting on internal stacks they had never come close to using.
// None of it was in the log.

// heap_caps_get_info(MALLOC_CAP_INTERNAL), in the shape the admission core
// judges. Free-block count included: it is what separates a shattered heap from
// a full one.
InternalHeapSnapshot CaptureInternalHeap();

// One line: internal totals plus the DMA-capable subset, which is the class the
// display draw buffer, the codec's I2S descriptors and the WiFi buffers compete
// for and which no PSRAM can stand in for.
void LogInternalHeapSummary(const char* phase);

// One line per task whose stack is in internal RAM, with the headroom it has
// never used. Task stacks are the largest firmware-controlled internal
// consumer — every FreeRTOS stack comes from pvPortMalloc(), i.e. always
// MALLOC_CAP_INTERNAL unless the creator explicitly asked for PSRAM — and a
// stack sized for a worst case that never happens is internal RAM nobody can
// see being wasted.
//
// Reports headroom rather than allocated size on purpose: ESP-IDF exposes the
// high-water mark for every task, but the stack's total size only through the
// heap block behind pxStackBase, and asking the heap about a statically
// allocated stack asserts. A diagnostic must not be able to panic the device it
// is diagnosing.
void LogInternalStackLedger(const char* phase);

// Both of the above, for the moment a session could not be built.
void LogInternalMemoryLedger(const char* phase);

}  // namespace eidolon

#endif  // EIDOLON_INTERNAL_MEMORY_REPORT_H_
