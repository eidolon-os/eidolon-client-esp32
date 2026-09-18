#include "eidolon/control_protocol.h"
#include "eidolon/hub_clock.h"
#include <cassert>
#include <string>
using namespace eidolon;
namespace {
constexpr int64_t kUtc = 1789748924000LL;
std::string Command(int64_t ts, const std::string& ttl = "5000") {
    return "{\"v\":1,\"kind\":\"cmd\",\"op\":\"expression.play\",\"ts\":" +
        std::to_string(ts) + ",\"ttl_ms\":" + ttl + "}";
}
}
int main() {
    HubClock clock;
    auto cmd = ParseControlCommand(Command(kUtc), clock.Now(100));
    assert(cmd.valid && !cmd.clock_known && !cmd.bounded_deadline && !cmd.expired);
    // Cold boot: system time is irrelevant. Authenticated Date establishes a
    // conservative UTC bound and local monotonic time advances it.
    clock.Observe(kUtc, 100, 120);
    assert(clock.Now(120) == kUtc + 1019);
    cmd = ParseControlCommand(Command(kUtc), clock.Now(120));
    assert(cmd.clock_known && cmd.bounded_deadline && !cmd.expired);
    assert(ParseControlCommand(Command(kUtc), clock.Now(5000)).expired);
    // A queued command does not get a fresh TTL on arrival.
    assert(ParseControlCommand(Command(kUtc - 20000), clock.Now(120)).expired);
    assert(!ParseControlCommand(Command(kUtc + 10000), clock.Now(120)).bounded_deadline);
    for (auto ttl : {"null", "true", "-1", "1.5", "15001", "1e999"}) {
        assert(!ParseControlCommand(Command(kUtc, ttl), clock.Now(120)).bounded_deadline);
    }
    cmd = ParseControlCommand(R"({"v":1,"kind":"cmd","op":"expression.play","ts":1e999,"ttl_ms":5})", clock.Now(120));
    assert(!cmd.bounded_deadline);
    // New configuration / boot does not inherit old Authority time.
    clock = HubClock{};
    assert(clock.Now(99999) == 0);
    clock.Observe(0, 100, 120);
    assert(clock.Now(120) == 0);
    clock.Observe(kUtc, 100, 120);
    assert(clock.Now(119) == 0); // monotonic reset
    // Network response delay is included, never grants stale packets more life.
    clock.Observe(kUtc, 100, 10100);
    assert(ParseControlCommand(Command(kUtc), clock.Now(10100)).expired);
}
