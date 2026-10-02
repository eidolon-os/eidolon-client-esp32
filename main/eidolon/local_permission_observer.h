#pragma once

#include <cstdint>

namespace eidolon {

// A transport permission change is a hint to re-read Owner configuration,
// never an authorization to open a microphone. Each new room starts a baseline.
class LocalPermissionObserver {
public:
    bool Observe(bool local, bool publish, bool subscribe, bool data) {
        if (!local) return false;
        const uint8_t next = publish | (subscribe << 1) | (data << 2);
        const bool changed = known_ && next != value_;
        value_ = next;
        known_ = true;
        return changed;
    }
    void Reset() { known_ = false; }

private:
    bool known_ = false;
    uint8_t value_ = 0;
};

}  // namespace eidolon
