#include "eidolon/pending_room_join.h"
#include <cassert>
#include <cstdio>

using namespace eidolon;
ControlCommand Command(const char* id) {
    ControlCommand command;
    command.id=id;
    command.op=kControlOpRoomJoin;
    return command;
}
int main() {
    PendingRoomJoin pending;
    assert(pending.Begin(Command("first"), "") == PendingRoomJoin::Admission::Started);
    pending.BindGeneration(7);
    // A second invitation must not steal the receipt of the first one.
    assert(pending.Begin(Command("second"), "presence") == PendingRoomJoin::Admission::Busy);
    assert(pending.Begin(Command("first"), "") == PendingRoomJoin::Admission::Retry);
    assert(pending.Begin(Command("first"), "presence") == PendingRoomJoin::Admission::Conflict);
    assert(pending.active());
    assert(!pending.Matches(6));
    assert(pending.Matches(7));
    const auto completed=pending.Complete();
    assert(completed && completed->id=="first");
    assert(!pending.Complete());
    assert(!pending.Matches(7));
    assert(!pending.active());
    // Both success and failure release the slot; neither replays a terminal ACK.
    assert(pending.Begin(Command("second"), "presence") == PendingRoomJoin::Admission::Started);
    pending.BindGeneration(8);
    assert(!pending.Matches(7));
    assert(pending.Complete()->id=="second");
    pending.BindGeneration(99); // a late bind cannot resurrect a completed request
    assert(!pending.Matches(99));
    assert(pending.Begin(Command("third"), "") == PendingRoomJoin::Admission::Started);
    assert(pending.Matches(9)); // generation is bound after the asynchronous join starts
    assert(pending.Complete()->id=="third");
    // Uncorrelated legacy requests must never be treated as idempotent retries.
    assert(pending.Begin(Command(""), "") == PendingRoomJoin::Admission::Started);
    assert(pending.Begin(Command(""), "") == PendingRoomJoin::Admission::Busy);
    assert(pending.Complete());
    assert(pending.Begin(Command("prepared"), "", true) == PendingRoomJoin::Admission::Started);
    assert(pending.control_request_id() == "prepared");
    assert(pending.Begin(Command("prepared"), "", false) == PendingRoomJoin::Admission::Conflict);
    assert(pending.Begin(Command("prepared"), "", true) == PendingRoomJoin::Admission::Retry);
    assert(pending.Complete()->id == "prepared");
    assert(pending.control_request_id().empty());
    assert(pending.Begin(Command("ordinary"), "") == PendingRoomJoin::Admission::Started);
    assert(pending.control_request_id().empty());
    assert(pending.Complete());
    std::puts("pending_room_join: PASS");
}
