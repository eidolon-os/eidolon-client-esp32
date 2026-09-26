#include "smarthome_store.h"

#include <utility>
#include <vector>

#include "smarthome_tiles.h"

namespace eidolon::smarthome {

ApplyOutcome SmartHomeStore::ApplySnapshot(Snapshot&& snapshot) {
    home_ = std::move(snapshot);
    has_snapshot_ = true;
    awaiting_snapshot_ = false;
    // A snapshot can only have come over the link, so the link is up even if
    // the transport's connect notification has not been delivered yet.
    link_up_ = true;
    sync_outstanding_ = false;
    // The activity line names a device of the old registry; a new one starts
    // without history rather than pointing at something that may be gone.
    activity_ = Activity{};
    ++generation_;
    return ApplyOutcome::Applied;
}

ApplyOutcome SmartHomeStore::ApplyDelta(const Delta& delta, int64_t now_utc_ms) {
    if (!has_snapshot_ || delta.revision != home_.revision) return ApplyOutcome::NeedSync;
    if (delta.seq <= home_.seq) return ApplyOutcome::Ignored;
    if (delta.seq != home_.seq + 1) return ApplyOutcome::NeedSync;
    // Resolve every change before touching anything: a delta is applied as a
    // whole or not at all, so the cache never mixes two host states.
    std::vector<Device*> targets;
    targets.reserve(delta.changes.size());
    for (const auto& change : delta.changes) {
        Device* device = nullptr;
        for (auto& candidate : home_.devices) {
            if (candidate.id == change.device_id) {
                device = &candidate;
                break;
            }
        }
        if (device == nullptr) return ApplyOutcome::NeedSync;
        targets.push_back(device);
    }
    if (targets.empty()) return ApplyOutcome::NeedSync;

    Activity activity;
    activity.valid = true;
    activity.utc_ms = now_utc_ms > 0 ? now_utc_ms : 0;
    activity.device_name = targets.front()->name;
    activity.change = DescribeChange(*targets.front(), delta.changes.front());
    activity.more = targets.size() - 1;
    activity.source = delta.source_label.empty() ? OriginLabel(delta.source_kind) : delta.source_label;

    for (size_t i = 0; i < targets.size(); ++i) {
        targets[i]->online = delta.changes[i].online;
        targets[i]->has_state = true;
        targets[i]->state = delta.changes[i].state;
    }
    home_.seq = delta.seq;
    activity_ = std::move(activity);
    ++generation_;
    return ApplyOutcome::Applied;
}

void SmartHomeStore::SetLinkUp(bool up) {
    if (up == link_up_) return;
    link_up_ = up;
    // Whatever arrived before the drop may have been followed by deltas that
    // never reached us; only a snapshot re-establishes the copy.
    if (up && has_snapshot_) awaiting_snapshot_ = true;
    // A request sent before the drop died with the link; the next one is due.
    if (!up) sync_outstanding_ = false;
    ++generation_;
}

bool SmartHomeStore::ClaimSyncRequest(int64_t now_ms, int64_t holdoff_ms) {
    if (sync_outstanding_ && now_ms >= last_sync_ms_ && now_ms - last_sync_ms_ < holdoff_ms) return false;
    sync_outstanding_ = true;
    last_sync_ms_ = now_ms;
    return true;
}

const Device* SmartHomeStore::FindDevice(const std::string& device_id) const {
    for (const auto& device : home_.devices) {
        if (device.id == device_id) return &device;
    }
    return nullptr;
}

const Area* SmartHomeStore::FindArea(const std::string& area_id) const {
    for (const auto& area : home_.areas) {
        if (area.id == area_id) return &area;
    }
    return nullptr;
}

}  // namespace eidolon::smarthome
