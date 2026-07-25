#include "device_event_builder.h"

#include "device_event_bus.h"
#include "eidolon_topics.h"

#include <cJSON.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace eidolon {
namespace {

struct CJsonDeleter {
    void operator()(cJSON* value) const { cJSON_Delete(value); }
};

bool IsBoundedId(const std::string& value, bool allow_empty)
{
    return (allow_empty || !value.empty()) && value.size() <= 96;
}

bool IsBoundedString(const std::string& value, size_t max_length)
{
    return !value.empty() && value.size() <= max_length;
}

bool IsComponentIdentifier(const std::string& value)
{
    if (value.empty() || value[0] < 'a' || value[0] > 'z') {
        return false;
    }
    for (size_t i = 1; i < value.size(); ++i) {
        const char character = value[i];
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') ||
                           character == '_' || character == '.' || character == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

bool AddString(cJSON* object, const char* key, const std::string& value)
{
    return cJSON_AddStringToObject(object, key, value.c_str()) != nullptr;
}

}  // namespace

std::string MakeDeviceEventId(const char* prefix, uint64_t monotonic_ms,
                              uint32_t random_value)
{
    if (prefix == nullptr || prefix[0] == '\0' || strlen(prefix) > 24) {
        return {};
    }
    char value[64] = {};
    const int length = std::snprintf(
        value, sizeof(value), "%s-%llu-%08lx", prefix,
        static_cast<unsigned long long>(monotonic_ms),
        static_cast<unsigned long>(random_value));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(value)) {
        return {};
    }
    return value;
}

std::string BuildDeviceEventJson(const DeviceEventEnvelope& event)
{
    if (!IsBoundedId(event.event_id, false) ||
        !IsBoundedId(event.flow_id, false) ||
        !IsBoundedId(event.causation_id, true) ||
        !IsBoundedString(event.type, 64) ||
        !IsBoundedString(event.source_device_id, 128) ||
        !IsBoundedString(event.source_component, 64) ||
        !IsComponentIdentifier(event.source_component) ||
        event.expires_at_ms <= event.occurred_at_ms ||
        event.expires_at_ms - event.occurred_at_ms > DeviceEventBus::kMaxTtlMs ||
        event.payload_json.empty()) {
        return {};
    }

    const char* parse_end = nullptr;
    std::unique_ptr<cJSON, CJsonDeleter> payload(
        cJSON_ParseWithLengthOpts(event.payload_json.data(),
                                  event.payload_json.size(), &parse_end, false));
    if (!payload || parse_end != event.payload_json.data() + event.payload_json.size() ||
        !cJSON_IsObject(payload.get())) {
        return {};
    }

    std::unique_ptr<cJSON, CJsonDeleter> root(cJSON_CreateObject());
    cJSON* source = cJSON_CreateObject();
    if (!root || source == nullptr ||
        cJSON_AddNumberToObject(root.get(), "schema_v",
                               kDeviceEventSchemaVersion) == nullptr ||
        cJSON_AddStringToObject(root.get(), "kind", "event") == nullptr ||
        !AddString(root.get(), "event_id", event.event_id) ||
        !AddString(root.get(), "flow_id", event.flow_id) ||
        !AddString(root.get(), "causation_id", event.causation_id) ||
        !AddString(root.get(), "type", event.type) ||
        !AddString(source, "device_id", event.source_device_id) ||
        !AddString(source, "component", event.source_component)) {
        cJSON_Delete(source);
        return {};
    }
    cJSON_AddItemToObject(root.get(), "source", source);
    if (cJSON_AddNumberToObject(root.get(), "occurred_at_ms",
                               static_cast<double>(event.occurred_at_ms)) == nullptr ||
        cJSON_AddNumberToObject(root.get(), "expires_at_ms",
                               static_cast<double>(event.expires_at_ms)) == nullptr) {
        return {};
    }
    cJSON_AddItemToObject(root.get(), "payload", payload.release());

    char* raw = cJSON_PrintUnformatted(root.get());
    if (raw == nullptr) {
        return {};
    }
    std::string json(raw);
    cJSON_free(raw);
    if (json.size() > DeviceEventBus::kMaxEventBytes) {
        return {};
    }
    return json;
}

}  // namespace eidolon
