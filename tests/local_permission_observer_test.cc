#include "eidolon/local_permission_observer.h"
#include <cassert>

int main() {
    eidolon::LocalPermissionObserver observer;
    assert(!observer.Observe(false, true, true, true));
    assert(!observer.Observe(true, false, true, true)); // receive-only join
    assert(!observer.Observe(false, true, true, true)); // remote publisher
    assert(!observer.Observe(true, false, true, true)); // duplicate snapshot
    assert(observer.Observe(true, true, true, true));  // live enable
    assert(!observer.Observe(true, true, true, true));
    assert(observer.Observe(true, false, true, true)); // live revoke
    assert(observer.Observe(true, false, false, true));
    assert(observer.Observe(true, false, false, false));
    observer.Reset();
    assert(!observer.Observe(true, true, true, true)); // next room's baseline
    assert(observer.Observe(true, false, true, true));
}
