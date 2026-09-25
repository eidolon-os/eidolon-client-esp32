#pragma once

#include <algorithm>
#include <cstdint>

namespace eidolon {

// Actor-owned monotonic deadlines. A retry or repeated Connected event cannot
// extend a visit; the ordinary session generation gate rejects old-room events.
class SharedTransportLease {
public:
    SharedTransportLease() = default;
    SharedTransportLease(int64_t now_us, int64_t now_ms,
                         int64_t join_deadline_ms, int64_t expires_at_ms)
        : join_deadline_us_(now_us + std::clamp<int64_t>(join_deadline_ms - now_ms, 0, 25000) * 1000),
          lease_deadline_us_(now_us + std::clamp<int64_t>(expires_at_ms - now_ms, 0, 3600000) * 1000) {}

    bool OnConnected(int64_t now_us) {
        if (now_us >= deadline_us() || now_us >= lease_deadline_us_) return false;
        connected_ = true;
        return true;
    }
    bool connected() const { return connected_; }
    int64_t deadline_us() const { return connected_ ? lease_deadline_us_ : join_deadline_us_; }

private:
    int64_t join_deadline_us_ = 0;
    int64_t lease_deadline_us_ = 0;
    bool connected_ = false;
};

}  // namespace eidolon
