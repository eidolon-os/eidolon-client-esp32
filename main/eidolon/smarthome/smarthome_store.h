#ifndef EIDOLON_SMARTHOME_STORE_H_
#define EIDOLON_SMARTHOME_STORE_H_

#include <cstdint>
#include <string>

#include "smarthome_model.h"

namespace eidolon::smarthome {

enum class ApplyOutcome : uint8_t {
    Applied,
    // Not applied, and nothing to do: a delta at or below the held seq is a
    // repeat of something already applied (or superseded by a snapshot).
    Ignored,
    // Not applied: the panel's copy cannot be brought forward by this message
    // (no snapshot yet, revision mismatch, sequence gap, a device the held
    // registry does not know). The caller sends smarthome.sync.
    NeedSync,
};

// The most recent state change, for the activity line.
struct Activity {
    bool valid = false;
    int64_t utc_ms = 0;          // 0 when the panel had no wall clock
    std::string device_name;     // first changed device
    std::string change;          // e.g. "已打开"
    size_t more = 0;             // further devices changed by the same delta
    std::string source;          // e.g. "面板语音"
};

// The panel's cache of the Owner's home (SDK PanelSnapshot/PanelDelta rules).
//
// Not thread-safe: the owner serializes every call with rendering. Nothing in
// here decides what a command does; it only records what the host reported.
class SmartHomeStore {
public:
    // Replaces everything, including revision and seq. Clears staleness (and
    // implies the link is up: nothing else could have delivered it).
    ApplyOutcome ApplySnapshot(Snapshot&& snapshot);

    // Applies only when revision equals the held revision and seq is exactly
    // one past the last one; atomically, or not at all. A seq at or below the
    // last is ignored; a revision mismatch or a gap needs a sync.
    ApplyOutcome ApplyDelta(const Delta& delta, int64_t now_utc_ms);

    // The channel carrying snapshots and deltas went down or came back. While
    // down the last state is kept and flagged stale; after it comes back the
    // copy stays stale until a new snapshot proves nothing was missed.
    void SetLinkUp(bool up);

    // Whether to send smarthome.sync now (monotonic ms): at most one request
    // per holdoff while no snapshot has answered the previous one, so a stream
    // of mismatched deltas cannot turn into a stream of sync requests.
    bool ClaimSyncRequest(int64_t now_ms, int64_t holdoff_ms = 2000);

    bool has_snapshot() const { return has_snapshot_; }
    bool link_up() const { return link_up_; }
    bool awaiting_snapshot() const { return awaiting_snapshot_; }
    // What the panel shows must not be acted on: dim tiles, refuse taps.
    bool stale() const { return has_snapshot_ && (!link_up_ || awaiting_snapshot_); }

    uint64_t revision() const { return home_.revision; }
    uint64_t seq() const { return home_.seq; }
    const Snapshot& home() const { return home_; }
    const Device* FindDevice(const std::string& device_id) const;
    const Area* FindArea(const std::string& area_id) const;
    const Activity& activity() const { return activity_; }
    // Bumped by every change a view would draw differently.
    uint32_t generation() const { return generation_; }

private:
    Snapshot home_;
    Activity activity_;
    bool has_snapshot_ = false;
    bool link_up_ = false;
    bool awaiting_snapshot_ = false;
    bool sync_outstanding_ = false;
    int64_t last_sync_ms_ = 0;
    uint32_t generation_ = 0;
};

}  // namespace eidolon::smarthome

#endif  // EIDOLON_SMARTHOME_STORE_H_
