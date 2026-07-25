#include "guard/owner_recognition_flow.h"

#include <algorithm>
#include <utility>

namespace eidolon {

OwnerRecognitionFlowTracker::OwnerRecognitionFlowTracker(
    OwnerRecognitionFlowConfig config)
    : config_(config)
{
    config_.match_timeout_ms =
        std::max<uint32_t>(200, std::min<uint32_t>(config_.match_timeout_ms, 3000));
    config_.flow_timeout_ms =
        std::max(config_.match_timeout_ms, std::min<uint32_t>(config_.flow_timeout_ms, 3000));
    config_.fresh_match_max_age_ms =
        std::min(config_.fresh_match_max_age_ms, config_.match_timeout_ms);
}

OwnerRecognitionRequestResult OwnerRecognitionFlowTracker::Request(
    const std::string& flow_id, const std::string& causation_id, uint64_t now_ms,
    bool owner_is_present, uint64_t last_owner_match_ms, uint32_t context)
{
    if (flow_id.empty() || flow_id.size() > 96 || causation_id.empty() ||
        causation_id.size() > 96) {
        return OwnerRecognitionRequestResult::Rejected;
    }
    if (IsKnownFlow(flow_id)) {
        return OwnerRecognitionRequestResult::Duplicate;
    }
    if (owner_is_present && IsFreshMatch(now_ms, last_owner_match_ms)) {
        RememberCompleted(flow_id);
        return OwnerRecognitionRequestResult::Confirmed;
    }
    if (pending_count_ >= pending_.size()) {
        return OwnerRecognitionRequestResult::Rejected;
    }
    pending_[pending_count_++] = {
        .flow_id = flow_id,
        .causation_id = causation_id,
        .requested_at_ms = now_ms,
        .match_deadline_ms = now_ms + config_.match_timeout_ms,
        .flow_deadline_ms = now_ms + config_.flow_timeout_ms,
        .context = context,
        .face_matched = false,
    };
    return OwnerRecognitionRequestResult::Pending;
}

void OwnerRecognitionFlowTracker::RecordFaceMatch(uint64_t now_ms)
{
    for (size_t i = 0; i < pending_count_; ++i) {
        if (now_ms <= pending_[i].match_deadline_ms) {
            pending_[i].face_matched = true;
        }
    }
}

std::vector<OwnerRecognitionFlow>
OwnerRecognitionFlowTracker::CompleteIfOwnerPresent(uint64_t now_ms,
                                                     bool owner_is_present)
{
    std::vector<OwnerRecognitionFlow> completed;
    if (!owner_is_present) {
        return completed;
    }
    completed.reserve(pending_count_);
    size_t index = 0;
    while (index < pending_count_) {
        if (pending_[index].face_matched && now_ms <= pending_[index].flow_deadline_ms) {
            RememberCompleted(pending_[index].flow_id);
            completed.push_back(std::move(pending_[index]));
            RemoveAt(index);
        } else {
            ++index;
        }
    }
    return completed;
}

void OwnerRecognitionFlowTracker::Expire(uint64_t now_ms)
{
    size_t index = 0;
    while (index < pending_count_) {
        const auto& flow = pending_[index];
        const bool expired =
            now_ms > flow.flow_deadline_ms ||
            (!flow.face_matched && now_ms > flow.match_deadline_ms);
        if (expired) {
            RememberCompleted(flow.flow_id);
            RemoveAt(index);
        } else {
            ++index;
        }
    }
}

void OwnerRecognitionFlowTracker::Clear()
{
    for (size_t i = 0; i < pending_count_; ++i) {
        pending_[i] = {};
    }
    pending_count_ = 0;
    for (size_t i = 0; i < recent_count_; ++i) {
        recent_flow_ids_[i].clear();
    }
    recent_count_ = 0;
    next_recent_index_ = 0;
}

bool OwnerRecognitionFlowTracker::NeedsFaceSample(uint64_t now_ms) const
{
    for (size_t i = 0; i < pending_count_; ++i) {
        if (!pending_[i].face_matched && now_ms <= pending_[i].match_deadline_ms) {
            return true;
        }
    }
    return false;
}

bool OwnerRecognitionFlowTracker::IsFreshMatch(uint64_t now_ms,
                                               uint64_t last_owner_match_ms) const
{
    return last_owner_match_ms != 0 && now_ms >= last_owner_match_ms &&
           now_ms - last_owner_match_ms <= config_.fresh_match_max_age_ms;
}

bool OwnerRecognitionFlowTracker::IsKnownFlow(const std::string& flow_id) const
{
    for (size_t i = 0; i < pending_count_; ++i) {
        if (pending_[i].flow_id == flow_id) {
            return true;
        }
    }
    for (size_t i = 0; i < recent_count_; ++i) {
        if (recent_flow_ids_[i] == flow_id) {
            return true;
        }
    }
    return false;
}

void OwnerRecognitionFlowTracker::RememberCompleted(const std::string& flow_id)
{
    recent_flow_ids_[next_recent_index_] = flow_id;
    next_recent_index_ = (next_recent_index_ + 1) % recent_flow_ids_.size();
    recent_count_ = std::min(recent_count_ + 1, recent_flow_ids_.size());
}

void OwnerRecognitionFlowTracker::RemoveAt(size_t index)
{
    if (index >= pending_count_) {
        return;
    }
    for (size_t i = index + 1; i < pending_count_; ++i) {
        pending_[i - 1] = std::move(pending_[i]);
    }
    pending_[--pending_count_] = {};
}

}  // namespace eidolon
