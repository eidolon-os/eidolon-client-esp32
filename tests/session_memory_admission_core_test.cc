#include <cassert>
#include <cstddef>
#include <string>

#include "eidolon/session_memory_admission_core.h"

namespace {

using eidolon::EngineInternalMemoryRequirement;
using eidolon::InternalHeapSnapshot;
using eidolon::SessionMemoryRetryLedger;
using eidolon::SessionMemoryVerdict;

// The requirement the firmware actually ships with, so these tests are anchored
// to the blocks the device asks the internal heap for, not to invented ones.
EngineInternalMemoryRequirement Requirement()
{
    return eidolon::LiveKitEngineInternalMemoryRequirement();
}

InternalHeapSnapshot Snapshot(std::size_t free_bytes, std::size_t largest_block,
                              std::size_t free_blocks = 4)
{
    InternalHeapSnapshot snapshot;
    snapshot.free_bytes = free_bytes;
    snapshot.largest_free_block = largest_block;
    snapshot.min_free_bytes = free_bytes;
    snapshot.free_blocks = free_blocks;
    return snapshot;
}

// The engine's two big allocations are separate xQueueCreate / xTaskCreate
// calls, so what must fit in one piece is the LARGER of the two — not their sum,
// and not the total. Getting this wrong is what made the field failure look like
// "22 KB free, so memory is fine".
void TestRequirementSeparatesContiguousFromTotal()
{
    const EngineInternalMemoryRequirement req = Requirement();
    assert(req.event_queue_bytes > 0);
    assert(req.task_stack_bytes > 0);
    assert(eidolon::LargestContiguousRequirement(req) ==
           (req.event_queue_bytes > req.task_stack_bytes ? req.event_queue_bytes
                                                         : req.task_stack_bytes));
    assert(eidolon::TotalRequirement(req) >
           eidolon::LargestContiguousRequirement(req));
    // The shipped engine wants a 32-slot queue of ~296-byte events, which is
    // larger than its 8 KiB task stack; both exceed 8 KiB in one piece.
    assert(eidolon::LargestContiguousRequirement(req) > 8 * 1024);
    assert(req.event_queue_bytes > req.task_stack_bytes);
}

void TestRoomyHeapIsAdmitted()
{
    const EngineInternalMemoryRequirement req = Requirement();
    const InternalHeapSnapshot heap =
        Snapshot(eidolon::TotalRequirement(req) * 3, eidolon::TotalRequirement(req) * 2, 1);
    assert(eidolon::JudgeSessionMemory(heap, req) == SessionMemoryVerdict::Sufficient);
    assert(eidolon::ContiguousShortfallBytes(heap, req) == 0);
    assert(eidolon::TotalShortfallBytes(heap, req) == 0);
}

// Exactly enough, in exactly one piece, is enough. The admission rule is a
// floor, so it must not refuse a heap that meets it.
void TestExactFitIsAdmitted()
{
    const EngineInternalMemoryRequirement req = Requirement();
    const InternalHeapSnapshot heap = Snapshot(eidolon::TotalRequirement(req),
                                               eidolon::LargestContiguousRequirement(req), 3);
    assert(eidolon::JudgeSessionMemory(heap, req) == SessionMemoryVerdict::Sufficient);
}

// The field failure, verbatim: internal_free=22299 largest_block=7680. The total
// clears the floor, so from one sample this is a shattered heap rather than a
// board that provably cannot hold a session.
void TestFieldFailureIsFragmentationNotExhaustion()
{
    const EngineInternalMemoryRequirement req = Requirement();
    const InternalHeapSnapshot heap = Snapshot(22299, 7680, 9);
    assert(eidolon::JudgeSessionMemory(heap, req) == SessionMemoryVerdict::Fragmented);
    assert(eidolon::ContiguousShortfallBytes(heap, req) ==
           eidolon::LargestContiguousRequirement(req) - 7680);
    assert(eidolon::TotalShortfallBytes(heap, req) == 0);
}

// Below the floor in total, no arrangement of the free space can satisfy the
// engine, so there is nothing to wait for.
void TestBelowTheFloorIsExhaustion()
{
    const EngineInternalMemoryRequirement req = Requirement();
    const std::size_t total = eidolon::TotalRequirement(req);
    const InternalHeapSnapshot heap = Snapshot(total - 1, total - 1, 1);
    assert(eidolon::JudgeSessionMemory(heap, req) == SessionMemoryVerdict::Exhausted);
    assert(eidolon::TotalShortfallBytes(heap, req) == 1);
    assert(eidolon::ContiguousShortfallBytes(heap, req) == 0);

    // The realistic shape: short on both counts at once. The total is what
    // decides, because "fragmented" would send a reader hunting a hole that
    // could never have existed.
    const InternalHeapSnapshot shattered = Snapshot(total / 2, 5000, 11);
    assert(eidolon::ContiguousShortfallBytes(shattered, req) > 0);
    assert(eidolon::TotalShortfallBytes(shattered, req) > 0);
    assert(eidolon::JudgeSessionMemory(shattered, req) == SessionMemoryVerdict::Exhausted);
}

// Exhaustion is terminal on the first refusal: waiting does not add internal
// RAM that the working set does not have.
void TestExhaustionStopsRetryingImmediately()
{
    const EngineInternalMemoryRequirement req = Requirement();
    const std::size_t total = eidolon::TotalRequirement(req);
    SessionMemoryRetryLedger ledger;
    assert(ledger.retry_worthwhile());
    assert(!ledger.ceiling_reached());

    assert(!ledger.RecordRefusal(Snapshot(total / 2, total / 2, 1), req));
    assert(ledger.ceiling_reached());
    assert(!ledger.retry_worthwhile());
    assert(ledger.consecutive_refusals() == 1);
    assert(ledger.last_verdict() == SessionMemoryVerdict::Exhausted);
}

// A fragmented heap earns retries only while the largest block keeps growing. A
// shortfall that has not moved for three attempts is a standing condition, not a
// transient one, and must stop asking — the 30 s timer in the field log would
// otherwise rediscover the same ceiling for the rest of the device's uptime.
void TestFragmentationRetriesUntilProgressStalls()
{
    const EngineInternalMemoryRequirement req = Requirement();
    const std::size_t needed = eidolon::LargestContiguousRequirement(req);
    const std::size_t roomy_total = eidolon::TotalRequirement(req) * 2;
    SessionMemoryRetryLedger ledger;

    assert(ledger.RecordRefusal(Snapshot(roomy_total, needed - 6000, 12), req));
    assert(ledger.consecutive_refusals() == 1);
    assert(ledger.attempts_since_progress() == 0);
    // Real progress: the largest block grew, so the next attempt is justified.
    assert(ledger.RecordRefusal(Snapshot(roomy_total, needed - 3000, 10), req));
    assert(ledger.attempts_since_progress() == 0);
    // Three attempts that did not improve on the best block ever seen.
    assert(ledger.RecordRefusal(Snapshot(roomy_total, needed - 3000, 10), req));
    assert(ledger.attempts_since_progress() == 1);
    assert(ledger.RecordRefusal(Snapshot(roomy_total, needed - 3100, 10), req));
    assert(ledger.attempts_since_progress() == 2);
    assert(!ledger.RecordRefusal(Snapshot(roomy_total, needed - 3000, 10), req));
    assert(ledger.attempts_since_progress() == 3);
    assert(ledger.ceiling_reached());
    assert(ledger.best_largest_free_block() == needed - 3000);
    assert(ledger.consecutive_refusals() == 5);
}

// Noise is not progress. A block that grows by a few bytes between attempts must
// not buy another five attempts, or the stall rule never fires.
void TestTinyImprovementIsNotProgress()
{
    const EngineInternalMemoryRequirement req = Requirement();
    const std::size_t needed = eidolon::LargestContiguousRequirement(req);
    const std::size_t roomy_total = eidolon::TotalRequirement(req) * 2;
    SessionMemoryRetryLedger ledger;

    assert(ledger.RecordRefusal(Snapshot(roomy_total, needed - 4000, 12), req));
    assert(ledger.RecordRefusal(Snapshot(roomy_total, needed - 3999, 12), req));
    assert(ledger.attempts_since_progress() == 1);
    assert(ledger.RecordRefusal(Snapshot(roomy_total, needed - 3998, 12), req));
    assert(ledger.attempts_since_progress() == 2);
    assert(!ledger.RecordRefusal(Snapshot(roomy_total, needed - 3997, 12), req));
    assert(ledger.ceiling_reached());
}

// A room that does get built clears the ledger: the next memory refusal is a new
// fact about a new moment, not attempt six of the old one.
void TestSuccessClearsTheLedger()
{
    const EngineInternalMemoryRequirement req = Requirement();
    const std::size_t needed = eidolon::LargestContiguousRequirement(req);
    const std::size_t total = eidolon::TotalRequirement(req);
    SessionMemoryRetryLedger ledger;
    ledger.RecordRefusal(Snapshot(total / 2, total / 2, 1), req);
    assert(ledger.ceiling_reached());

    ledger.Reset();
    assert(!ledger.ceiling_reached());
    assert(ledger.retry_worthwhile());
    assert(ledger.consecutive_refusals() == 0);
    assert(ledger.best_largest_free_block() == 0);

    assert(ledger.RecordRefusal(Snapshot(total * 2, needed - 100, 12), req));
}

// Whatever the device does about it, a person reads the log first, so the
// verdict has to have a name in it.
void TestVerdictsAreNamed()
{
    assert(std::string(eidolon::SessionMemoryVerdictName(
               SessionMemoryVerdict::Sufficient)) == "sufficient");
    assert(std::string(eidolon::SessionMemoryVerdictName(
               SessionMemoryVerdict::Fragmented)) == "fragmented");
    assert(std::string(eidolon::SessionMemoryVerdictName(
               SessionMemoryVerdict::Exhausted)) == "exhausted");
}

}  // namespace

int main()
{
    TestRequirementSeparatesContiguousFromTotal();
    TestRoomyHeapIsAdmitted();
    TestExactFitIsAdmitted();
    TestFieldFailureIsFragmentationNotExhaustion();
    TestBelowTheFloorIsExhaustion();
    TestExhaustionStopsRetryingImmediately();
    TestFragmentationRetriesUntilProgressStalls();
    TestTinyImprovementIsNotProgress();
    TestSuccessClearsTheLedger();
    TestVerdictsAreNamed();
    return 0;
}
