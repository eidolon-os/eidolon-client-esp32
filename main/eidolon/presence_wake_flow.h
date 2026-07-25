#ifndef EIDOLON_PRESENCE_WAKE_FLOW_H_
#define EIDOLON_PRESENCE_WAKE_FLOW_H_

#include <cstdint>
#include <string>

namespace eidolon {

struct PresenceWakeFlowConfig {
    uint32_t flow_timeout_ms = 3000;
    uint32_t cooldown_ms = 30000;
};

enum class PresenceWakeStartResult {
    Ignored,
    Started,
};

enum class PresenceWakeConfirmationResult {
    Ignored,
    Matched,
};

class PresenceWakeFlowTracker {
public:
    explicit PresenceWakeFlowTracker(PresenceWakeFlowConfig config = {});

    PresenceWakeStartResult Start(const std::string& flow_id,
                                  const std::string& ambient_event_id,
                                  uint64_t now_ms);
    PresenceWakeConfirmationResult Confirm(const std::string& flow_id,
                                           uint64_t now_ms);
    bool Expire(uint64_t now_ms);
    bool Cancel();

    bool waiting() const { return !pending_flow_id_.empty(); }
    const std::string& pending_flow_id() const { return pending_flow_id_; }
    const std::string& ambient_event_id() const { return ambient_event_id_; }
    uint64_t deadline_ms() const { return deadline_ms_; }

private:
    PresenceWakeFlowConfig config_;
    std::string pending_flow_id_;
    std::string ambient_event_id_;
    uint64_t deadline_ms_ = 0;
    uint64_t last_started_ms_ = 0;
};

}  // namespace eidolon

#endif  // EIDOLON_PRESENCE_WAKE_FLOW_H_
