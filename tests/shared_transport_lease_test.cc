#include <cassert>
#include "eidolon/shared_transport_lease.h"

int main() {
    using eidolon::SharedTransportLease;
    SharedTransportLease pending(5000, 1000, 2000, 4000);
    assert(!pending.connected());
    assert(pending.deadline_us() == 1005000);
    assert(pending.OnConnected(1004999));
    assert(pending.connected());
    assert(pending.deadline_us() == 3005000);
    assert(pending.OnConnected(1005001));  // duplicate does not extend the lease
    assert(pending.deadline_us() == 3005000);
    assert(!pending.OnConnected(3005000));
    SharedTransportLease late(5000, 1000, 2000, 4000);
    assert(!late.OnConnected(1005000)); // Connected racing timeout cannot revive admission
    assert(!late.connected());
    SharedTransportLease expired(5000, 4000, 2000, 3000);
    assert(!expired.OnConnected(5000));
    SharedTransportLease bounded(0, 1000, 100000, 100000000);
    assert(bounded.deadline_us() == 25000000);
    assert(bounded.OnConnected(1));
    assert(bounded.deadline_us() == 3600000000LL);
    SharedTransportLease empty;
    assert(!empty.OnConnected(0));
}
