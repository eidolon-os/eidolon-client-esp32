#ifndef EIDOLON_DEVICE_EVENT_BUS_H_
#define EIDOLON_DEVICE_EVENT_BUS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace eidolon {

struct DeviceEventMessage {
    std::string event_id;
    std::string flow_id;
    std::string causation_id;
    std::string type;
    std::string source_device_id;
    std::string source_component;
    uint64_t occurred_at_ms = 0;
    uint64_t expires_at_ms = 0;
    std::string payload_json;
};

enum class DeviceEventDispatchResult {
    Invalid,
    Expired,
    Duplicate,
    Unhandled,
    Handled,
};

class DeviceEventBus {
public:
    using Handler = std::function<void(const DeviceEventMessage&)>;

    static constexpr size_t kMaxEventBytes = 2048;
    static constexpr uint64_t kMaxTtlMs = 3000;
    static constexpr size_t kMaxHandlers = 8;
    static constexpr size_t kMaxRecentEvents = 64;

    explicit DeviceEventBus(size_t recent_event_limit = kMaxRecentEvents);

    bool RegisterHandler(const std::string& type, Handler handler);
    DeviceEventDispatchResult Dispatch(const std::string& json, uint64_t now_epoch_ms = 0);

private:
    struct HandlerEntry {
        std::string type;
        Handler handler;
    };

    bool Parse(const std::string& json, DeviceEventMessage* event) const;
    bool IsDuplicate(const std::string& event_id);

    std::array<HandlerEntry, kMaxHandlers> handlers_ = {};
    size_t handler_count_ = 0;
    size_t recent_event_limit_;
    std::deque<std::string> recent_event_ids_;
};

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_EVENT_BUS_H_
