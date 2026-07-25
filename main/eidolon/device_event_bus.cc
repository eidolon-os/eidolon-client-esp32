#include "device_event_bus.h"

#include "eidolon_topics.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace eidolon {

namespace {

constexpr size_t kMaxEventIdLength = 96;
constexpr size_t kMaxDeviceIdLength = 128;
constexpr size_t kMaxComponentLength = 64;

bool IsBoundedString(const cJSON* item, size_t min_length, size_t max_length)
{
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    const size_t length = strlen(item->valuestring);
    return length >= min_length && length <= max_length;
}

bool IsUint64(const cJSON* item, bool allow_zero)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        std::floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    const double minimum = allow_zero ? 0.0 : 1.0;
    return item->valuedouble >= minimum &&
           item->valuedouble <= static_cast<double>(std::numeric_limits<uint64_t>::max());
}

bool IsComponentIdentifier(const char* value)
{
    if (value == nullptr || value[0] < 'a' || value[0] > 'z') {
        return false;
    }
    for (const char* cursor = value + 1; *cursor != '\0'; ++cursor) {
        const bool valid = (*cursor >= 'a' && *cursor <= 'z') ||
                           (*cursor >= '0' && *cursor <= '9') ||
                           *cursor == '_' || *cursor == '.' || *cursor == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

bool HasExactFields(const cJSON* object, const char* const* known_fields,
                    size_t known_field_count)
{
    if (!cJSON_IsObject(object) ||
        static_cast<size_t>(cJSON_GetArraySize(object)) != known_field_count) {
        return false;
    }
    std::array<size_t, 10> occurrences = {};
    if (known_field_count > occurrences.size()) {
        return false;
    }
    for (const cJSON* item = object->child; item != nullptr; item = item->next) {
        bool found = false;
        for (size_t i = 0; i < known_field_count; ++i) {
            if (strcmp(item->string ? item->string : "", known_fields[i]) == 0) {
                ++occurrences[i];
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return std::all_of(occurrences.begin(), occurrences.begin() + known_field_count,
                       [](size_t count) { return count == 1; });
}

std::string PrintJson(const cJSON* item)
{
    char* raw = cJSON_PrintUnformatted(item);
    if (raw == nullptr) {
        return {};
    }
    std::string json(raw);
    cJSON_free(raw);
    return json;
}

}  // namespace

DeviceEventBus::DeviceEventBus(size_t recent_event_limit)
    : recent_event_limit_(std::max<size_t>(
          1, std::min(recent_event_limit, kMaxRecentEvents)))
{
}

bool DeviceEventBus::RegisterHandler(const std::string& type, Handler handler)
{
    if (type.empty() || type.size() > kMaxComponentLength || !handler) {
        return false;
    }
    for (size_t i = 0; i < handler_count_; ++i) {
        if (handlers_[i].type == type) {
            handlers_[i].handler = std::move(handler);
            return true;
        }
    }
    if (handler_count_ >= handlers_.size()) {
        return false;
    }
    handlers_[handler_count_++] = {type, std::move(handler)};
    return true;
}

DeviceEventDispatchResult DeviceEventBus::Dispatch(const std::string& json,
                                                   uint64_t now_epoch_ms)
{
    DeviceEventMessage event;
    if (!Parse(json, &event)) {
        return DeviceEventDispatchResult::Invalid;
    }
    if (now_epoch_ms != 0 && event.expires_at_ms <= now_epoch_ms) {
        return DeviceEventDispatchResult::Expired;
    }
    if (IsDuplicate(event.event_id)) {
        return DeviceEventDispatchResult::Duplicate;
    }
    for (size_t i = 0; i < handler_count_; ++i) {
        if (handlers_[i].type == event.type) {
            handlers_[i].handler(event);
            return DeviceEventDispatchResult::Handled;
        }
    }
    return DeviceEventDispatchResult::Unhandled;
}

bool DeviceEventBus::Parse(const std::string& json, DeviceEventMessage* event) const
{
    if (event == nullptr || json.empty() || json.size() > kMaxEventBytes) {
        return false;
    }
    const char* parse_end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(json.data(), json.size(), &parse_end, false);
    if (root == nullptr || parse_end != json.data() + json.size() ||
        !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    static constexpr const char* kEnvelopeFields[] = {
        "schema_v", "kind", "event_id", "flow_id", "causation_id",
        "type", "source", "occurred_at_ms", "expires_at_ms", "payload",
    };
    static constexpr const char* kSourceFields[] = {
        "device_id",
        "component",
    };

    const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema_v");
    const cJSON* kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const cJSON* event_id = cJSON_GetObjectItemCaseSensitive(root, "event_id");
    const cJSON* flow_id = cJSON_GetObjectItemCaseSensitive(root, "flow_id");
    const cJSON* causation_id = cJSON_GetObjectItemCaseSensitive(root, "causation_id");
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON* source = cJSON_GetObjectItemCaseSensitive(root, "source");
    const cJSON* occurred = cJSON_GetObjectItemCaseSensitive(root, "occurred_at_ms");
    const cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires_at_ms");
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON* source_device_id =
        cJSON_GetObjectItemCaseSensitive(source, "device_id");
    const cJSON* source_component =
        cJSON_GetObjectItemCaseSensitive(source, "component");

    const bool valid =
        HasExactFields(root, kEnvelopeFields, std::size(kEnvelopeFields)) &&
        cJSON_IsNumber(schema) && schema->valuedouble == kDeviceEventSchemaVersion &&
        cJSON_IsString(kind) && strcmp(kind->valuestring, "event") == 0 &&
        IsBoundedString(event_id, 1, kMaxEventIdLength) &&
        IsBoundedString(flow_id, 1, kMaxEventIdLength) &&
        IsBoundedString(causation_id, 0, kMaxEventIdLength) &&
        IsBoundedString(type, 1, kMaxComponentLength) &&
        HasExactFields(source, kSourceFields, std::size(kSourceFields)) &&
        IsBoundedString(source_device_id, 1, kMaxDeviceIdLength) &&
        IsBoundedString(source_component, 1, kMaxComponentLength) &&
        IsComponentIdentifier(source_component->valuestring) &&
        IsUint64(occurred, true) && IsUint64(expires, false) &&
        cJSON_IsObject(payload);
    if (!valid) {
        cJSON_Delete(root);
        return false;
    }

    const uint64_t occurred_at_ms = static_cast<uint64_t>(occurred->valuedouble);
    const uint64_t expires_at_ms = static_cast<uint64_t>(expires->valuedouble);
    if (expires_at_ms <= occurred_at_ms ||
        expires_at_ms - occurred_at_ms > kMaxTtlMs) {
        cJSON_Delete(root);
        return false;
    }

    event->event_id = event_id->valuestring;
    event->flow_id = flow_id->valuestring;
    event->causation_id = causation_id->valuestring;
    event->type = type->valuestring;
    event->source_device_id = source_device_id->valuestring;
    event->source_component = source_component->valuestring;
    event->occurred_at_ms = occurred_at_ms;
    event->expires_at_ms = expires_at_ms;
    event->payload_json = PrintJson(payload);
    const bool payload_printed = !event->payload_json.empty();
    cJSON_Delete(root);
    return payload_printed;
}

bool DeviceEventBus::IsDuplicate(const std::string& event_id)
{
    for (size_t index = 0; index < recent_event_count_; ++index) {
        if (recent_event_ids_[index] != event_id) {
            continue;
        }
        // Preserve LRU behavior: a duplicate becomes the newest retained ID.
        std::string retained = std::move(recent_event_ids_[index]);
        for (size_t next = index + 1; next < recent_event_count_; ++next) {
            recent_event_ids_[next - 1] = std::move(recent_event_ids_[next]);
        }
        recent_event_ids_[recent_event_count_ - 1] = std::move(retained);
        return true;
    }

    if (recent_event_count_ < recent_event_limit_) {
        recent_event_ids_[recent_event_count_++] = event_id;
        return false;
    }

    for (size_t index = 1; index < recent_event_count_; ++index) {
        recent_event_ids_[index - 1] = std::move(recent_event_ids_[index]);
    }
    recent_event_ids_[recent_event_count_ - 1] = event_id;
    return false;
}

}  // namespace eidolon
