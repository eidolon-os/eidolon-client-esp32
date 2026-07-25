#ifndef EIDOLON_DEVICE_EVENT_BUILDER_H_
#define EIDOLON_DEVICE_EVENT_BUILDER_H_

#include <cstdint>
#include <string>

namespace eidolon {

struct DeviceEventEnvelope {
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

std::string MakeDeviceEventId(const char* prefix, uint64_t monotonic_ms,
                              uint32_t random_value);
std::string BuildDeviceEventJson(const DeviceEventEnvelope& event);

}  // namespace eidolon

#endif  // EIDOLON_DEVICE_EVENT_BUILDER_H_
