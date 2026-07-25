#ifndef EIDOLON_OWNER_RECOGNITION_FLOW_H_
#define EIDOLON_OWNER_RECOGNITION_FLOW_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eidolon {

struct OwnerRecognitionFlowConfig {
    uint32_t match_timeout_ms = 1500;
    uint32_t flow_timeout_ms = 3000;
    uint32_t fresh_match_max_age_ms = 500;
};

struct OwnerRecognitionFlow {
    std::string flow_id;
    std::string causation_id;
    uint64_t requested_at_ms = 0;
    uint64_t match_deadline_ms = 0;
    uint64_t flow_deadline_ms = 0;
    uint32_t context = 0;
    bool face_matched = false;
};

enum class OwnerRecognitionRequestResult {
    Rejected,
    Duplicate,
    Pending,
    Confirmed,
};

class OwnerRecognitionFlowTracker {
public:
    static constexpr size_t kMaxPendingFlows = 8;
    static constexpr size_t kMaxRecentFlows = 32;

    explicit OwnerRecognitionFlowTracker(OwnerRecognitionFlowConfig config = {});

    OwnerRecognitionRequestResult Request(const std::string& flow_id,
                                          const std::string& causation_id,
                                          uint64_t now_ms,
                                          bool owner_is_present,
                                          uint64_t last_owner_match_ms,
                                          uint32_t context = 0);
    void RecordFaceMatch(uint64_t now_ms);
    std::vector<OwnerRecognitionFlow> CompleteIfOwnerPresent(uint64_t now_ms,
                                                             bool owner_is_present);
    void Expire(uint64_t now_ms);
    void Clear();

    bool HasPending() const { return pending_count_ != 0; }
    bool NeedsFaceSample(uint64_t now_ms) const;
    size_t pending_count() const { return pending_count_; }

private:
    OwnerRecognitionFlowConfig config_;
    std::array<OwnerRecognitionFlow, kMaxPendingFlows> pending_ = {};
    size_t pending_count_ = 0;
    std::array<std::string, kMaxRecentFlows> recent_flow_ids_ = {};
    size_t recent_count_ = 0;
    size_t next_recent_index_ = 0;

    bool IsFreshMatch(uint64_t now_ms, uint64_t last_owner_match_ms) const;
    bool IsKnownFlow(const std::string& flow_id) const;
    void RememberCompleted(const std::string& flow_id);
    void RemoveAt(size_t index);
};

}  // namespace eidolon

#endif  // EIDOLON_OWNER_RECOGNITION_FLOW_H_
