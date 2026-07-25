#include "presence_wake_flow.h"

#include <algorithm>

namespace eidolon {

PresenceWakeFlowTracker::PresenceWakeFlowTracker(PresenceWakeFlowConfig config)
    : config_(config)
{
    config_.flow_timeout_ms =
        std::max<uint32_t>(500, std::min<uint32_t>(config_.flow_timeout_ms, 3000));
    config_.cooldown_ms = std::max(config_.cooldown_ms, config_.flow_timeout_ms);
}

PresenceWakeStartResult PresenceWakeFlowTracker::Start(
    const std::string& flow_id, const std::string& ambient_event_id,
    uint64_t now_ms)
{
    if (waiting() || flow_id.empty() || flow_id.size() > 96 ||
        ambient_event_id.empty() || ambient_event_id.size() > 96 ||
        (last_started_ms_ != 0 && now_ms >= last_started_ms_ &&
         now_ms - last_started_ms_ < config_.cooldown_ms)) {
        return PresenceWakeStartResult::Ignored;
    }
    pending_flow_id_ = flow_id;
    ambient_event_id_ = ambient_event_id;
    deadline_ms_ = now_ms + config_.flow_timeout_ms;
    last_started_ms_ = now_ms;
    return PresenceWakeStartResult::Started;
}

PresenceWakeConfirmationResult PresenceWakeFlowTracker::Confirm(
    const std::string& flow_id, uint64_t now_ms)
{
    if (!waiting() || flow_id != pending_flow_id_ || now_ms > deadline_ms_) {
        return PresenceWakeConfirmationResult::Ignored;
    }
    Cancel();
    return PresenceWakeConfirmationResult::Matched;
}

bool PresenceWakeFlowTracker::Expire(uint64_t now_ms)
{
    if (!waiting() || now_ms < deadline_ms_) {
        return false;
    }
    return Cancel();
}

bool PresenceWakeFlowTracker::Cancel()
{
    if (!waiting()) {
        return false;
    }
    pending_flow_id_.clear();
    ambient_event_id_.clear();
    deadline_ms_ = 0;
    return true;
}

}  // namespace eidolon
