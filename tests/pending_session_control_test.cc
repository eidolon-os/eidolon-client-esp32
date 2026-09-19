#include "eidolon/pending_session_control.h"
#include <cassert>
#include <cstdio>

int main() {
    eidolon::PendingSessionControl pending;
    const std::string packet="session_started";
    // Reproduce the device trace: reliable data precedes Agent signalling by 10ms.
    assert(pending.Stage("agent",packet,1,100));
    assert(pending.Resolve("other",true,1,105).empty());
    assert(pending.Resolve("agent",true,1,110)==packet);
    assert(pending.Resolve("agent",true,1,111).empty()); // exactly once
    assert(pending.Stage("ordinary",packet,1,200));
    assert(pending.Resolve("ordinary",false,1,210).empty());
    assert(pending.Resolve("ordinary",true,1,211).empty()); // no later promotion replay
    assert(pending.Stage("agent",packet,1,300));
    assert(pending.Resolve("agent",true,2,310).empty()); // old room generation
    assert(pending.Stage("agent",packet,2,400));
    assert(pending.Resolve("agent",true,2,1400).empty()); // expired
    assert(pending.Stage("agent",packet,2,1500));
    assert(!pending.Stage("attacker","replace",2,1501)); // fixed one-packet bound
    assert(pending.Resolve("agent",true,2,1510)==packet);
    assert(!pending.Stage("",packet,2,1600));
    assert(!pending.Stage(std::string(129,'a'),packet,2,1600));
    assert(!pending.Stage("agent",std::string(1025,'a'),2,1600));
    assert(pending.Stage("agent",packet,2,1700));
    pending.Clear();
    assert(pending.Resolve("agent",true,2,1701).empty()); // disconnect clears it
    std::puts("pending_session_control: PASS");
}
