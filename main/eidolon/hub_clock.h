#ifndef EIDOLON_HUB_CLOCK_H_
#define EIDOLON_HUB_CLOCK_H_

#include <cstdint>
#include <limits>

namespace eidolon {

// A boot-local reading from authenticated Hub HTTP Date, advanced only by the
// local monotonic clock. This value travels with the configuration it came
// from; it is never persisted or shared between Owner configurations.
class HubClock {
public:
    void Observe(int64_t utc_ms, int64_t request_start_ms, int64_t received_ms) {
        utc_ms_ = 0;
        if (utc_ms < 1700000000000LL || request_start_ms < 0 ||
            received_ms < request_start_ms ||
            utc_ms > std::numeric_limits<int64_t>::max() - received_ms - 1000) return;
        // HTTP Date is rounded to seconds. Use the upper bound including the
        // round trip, so a delayed response cannot extend a command's TTL.
        utc_ms_ = utc_ms + 999 + (received_ms - request_start_ms);
        monotonic_ms_ = received_ms;
    }

    int64_t Now(int64_t monotonic_ms) const {
        if (!utc_ms_ || monotonic_ms < monotonic_ms_ ||
            monotonic_ms - monotonic_ms_ > std::numeric_limits<int64_t>::max() - utc_ms_) return 0;
        return utc_ms_ + (monotonic_ms - monotonic_ms_);
    }

private:
    int64_t utc_ms_ = 0;
    int64_t monotonic_ms_ = 0;
};

}  // namespace eidolon
#endif
