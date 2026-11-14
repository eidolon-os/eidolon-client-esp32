#include "device_settings_core.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <utility>

#include "eidolon/generated/device_settings_catalog.h"

namespace eidolon::device_settings {
namespace {

struct JsonDeleter {
    void operator()(cJSON* value) const { cJSON_Delete(value); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

std::string Print(const cJSON* value)
{
    char* raw = cJSON_PrintUnformatted(value);
    if (raw == nullptr) return {};
    std::string text(raw);
    cJSON_free(raw);
    return text;
}

bool OnlyKeys(const cJSON* object, std::initializer_list<const char*> keys)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON* item = object->child; item != nullptr; item = item->next) {
        if (item->string == nullptr) return false;
        bool known = false;
        for (const char* key : keys) known = known || std::strcmp(item->string, key) == 0;
        if (!known) return false;
        for (const cJSON* prior = object->child; prior != item; prior = prior->next) {
            if (std::strcmp(prior->string, item->string) == 0) return false;
        }
    }
    return true;
}

bool Identifier(const cJSON* value, std::string& out)
{
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
    const std::string text = value->valuestring;
    if (text.empty() || text.size() > 96) return false;
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-';
        if (!ok) return false;
    }
    out = text;
    return true;
}

bool Integer(const cJSON* value, double minimum, double maximum, int64_t& out)
{
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble)) return false;
    const double number = value->valuedouble;
    if (std::floor(number) != number || number < minimum || number > maximum) return false;
    out = static_cast<int64_t>(number);
    return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool OwnerControlled(const std::string& capability)
{
    return std::any_of(kOwnerControl.begin(), kOwnerControl.end(),
                       [&](std::string_view name) { return capability == name; });
}

enum class Scope { Bound, Unauthorized, Inactive };

Scope CheckScope(Caller caller, const std::string& session_id, int64_t revision,
                 const Binding& binding)
{
    switch (caller) {
    case Caller::Provider:
        if (binding.room.empty() || binding.standing_policy_revision == 0) return Scope::Inactive;
        return session_id == binding.room && revision == binding.standing_policy_revision
            ? Scope::Bound : Scope::Inactive;
    case Caller::Agent:
        if (binding.session_id.empty()) return Scope::Inactive;
        return session_id == binding.session_id && revision == binding.session_policy_revision
            ? Scope::Bound : Scope::Inactive;
    case Caller::Unknown:
        break;
    }
    return Scope::Unauthorized;
}

RpcAnswer Refuse(int code, const char* message)
{
    RpcAnswer answer;
    answer.error_code = code;
    answer.error_message = message;
    return answer;
}

RpcAnswer Answer(std::string payload)
{
    RpcAnswer answer;
    answer.ok = !payload.empty();
    answer.payload = std::move(payload);
    if (!answer.ok) {
        answer.error_code = kRpcInvalidOrInactive;
        answer.error_message = "answer_unavailable";
    }
    return answer;
}

// An ExecutionSnapshot, in the field names and vocabulary the SDK model reads.
std::string Snapshot(const std::string& execution_id, const char* state, int sequence,
                     const char* error, const char* result_key = nullptr, int result = 0)
{
    Json root(cJSON_CreateObject());
    if (!root) return {};
    cJSON_AddStringToObject(root.get(), "execution_id", execution_id.c_str());
    cJSON_AddStringToObject(root.get(), "state", state);
    cJSON_AddNumberToObject(root.get(), "sequence", sequence);
    cJSON* values = cJSON_AddObjectToObject(root.get(), "result");
    if (result_key != nullptr && values != nullptr) {
        cJSON_AddNumberToObject(values, result_key, result);
    }
    if (std::strcmp(state, "completed") == 0) {
        // A setting is done when the hardware holds the value, which is what
        // the registry promises for these contracts.
        cJSON_AddStringToObject(root.get(), "completion_basis", "configuration_applied");
    }
    cJSON_AddStringToObject(root.get(), "error", error);
    return Print(root.get());
}

}  // namespace

Runtime::Runtime(Actuators actuators) : actuators_(std::move(actuators))
{
    ledger_.reserve(kLedgerSize);
}

bool Runtime::HasSpeaker() const
{
    return bool(actuators_.read_volume) && bool(actuators_.apply_volume);
}

bool Runtime::HasDisplay() const
{
    return bool(actuators_.read_brightness) && bool(actuators_.apply_brightness);
}

std::string DeclaredActions(bool volume, bool brightness)
{
    std::string actions = volume ? kSpeakerActions : "";
    if (brightness) actions += std::string(actions.empty() ? "" : ",") + kDisplayActions;
    return actions;
}

std::string DeclaredProperties(bool volume, bool brightness)
{
    std::string properties = volume ? kSpeakerProperties : "";
    if (brightness) properties += std::string(properties.empty() ? "" : ",") + kDisplayProperties;
    return properties;
}

bool Runtime::IsMethod(const std::string& method)
{
    return method == kInvokeMethod || method == kStatusMethod || method == kCancelMethod ||
           method == kPropertiesReadMethod;
}

bool Runtime::Declares(const std::string& capability) const
{
    if (capability == kVolumeSet || capability == kVolumeAdjust) return HasSpeaker();
    if (capability == kBrightnessSet || capability == kBrightnessAdjust) return HasDisplay();
    return false;
}

RpcAnswer Runtime::Handle(const std::string& method, Caller caller, const std::string& payload,
                          const Binding& binding, int64_t now_ms, bool clock_set)
{
    if (caller == Caller::Unknown) return Refuse(kRpcUnauthorized, "unauthorized");
    if (payload.size() > kMaxRpcPayloadBytes) {
        return Refuse(kRpcUnauthorized, "oversized");
    }
    if (method == kInvokeMethod) return Invoke(caller, payload, binding, now_ms, clock_set);
    if (method == kStatusMethod || method == kCancelMethod) {
        // A setting completes inside its invocation, so there is never anything
        // left to cancel: both answer with the outcome already reached.
        return Status(caller, payload, binding);
    }
    if (method == kPropertiesReadMethod) return ReadProperties(caller);
    return Refuse(kRpcInvalidOrInactive, "unsupported_method");
}

RpcAnswer Runtime::Invoke(Caller caller, const std::string& payload, const Binding& binding,
                          int64_t now_ms, bool clock_set)
{
    Json root(cJSON_ParseWithLength(payload.data(), payload.size()));
    if (!root || !OnlyKeys(root.get(), {"execution_id", "session_id", "policy_revision",
                                        "protocol_version", "capability", "capability_version",
                                        "arguments", "expires_at_ms", "max_duration_ms"})) {
        return Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request");
    }
    std::string execution_id;
    std::string session_id;
    std::string capability;
    int64_t revision = 0;
    int64_t protocol = 0;
    int64_t version = 0;
    int64_t expires_at_ms = 0;
    int64_t max_duration_ms = 0;
    const cJSON* arguments = cJSON_GetObjectItemCaseSensitive(root.get(), "arguments");
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(root.get(), "capability");
    if (!Identifier(cJSON_GetObjectItemCaseSensitive(root.get(), "execution_id"), execution_id) ||
        !Identifier(cJSON_GetObjectItemCaseSensitive(root.get(), "session_id"), session_id) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(root.get(), "policy_revision"), 1, 4294967295.0,
                 revision) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(root.get(), "protocol_version"), 1, 1, protocol) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(root.get(), "capability_version"), 1, 1, version) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(root.get(), "expires_at_ms"), 1, 9007199254740991.0,
                 expires_at_ms) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(root.get(), "max_duration_ms"), 1, 30000,
                 max_duration_ms) ||
        !cJSON_IsString(name) || name->valuestring == nullptr || std::strlen(name->valuestring) > 128 ||
        !cJSON_IsObject(arguments)) {
        return Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request");
    }
    capability = name->valuestring;
    switch (CheckScope(caller, session_id, revision, binding)) {
    case Scope::Unauthorized: return Refuse(kRpcUnauthorized, "unauthorized");
    case Scope::Inactive: return Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request");
    case Scope::Bound: break;
    }

    const std::string request = std::string(caller == Caller::Agent ? "agent" : "provider") +
        "|" + session_id + "|" + std::to_string(revision) + "|" + capability + "|" +
        Print(arguments);
    if (const Entry* previous = Find(execution_id)) {
        if (previous->request != request) {
            return Answer(Snapshot(execution_id, "rejected", 0, "id_conflict"));
        }
        return Answer(previous->snapshot);
    }
    if (clock_set && (expires_at_ms <= now_ms || expires_at_ms > now_ms + 30000)) {
        return Answer(Snapshot(execution_id, "rejected", 0, "expired"));
    }
    const bool allowed = Declares(capability) &&
        (caller == Caller::Provider ? OwnerControlled(capability)
                                    : Contains(binding.session_capabilities, capability));
    if (!allowed) return Answer(Snapshot(execution_id, "rejected", 0, "unsupported_capability"));

    const bool volume = capability == kVolumeSet || capability == kVolumeAdjust;
    const bool adjust = capability == kVolumeAdjust || capability == kBrightnessAdjust;
    const char* field = adjust ? "change" : (volume ? "volume" : "brightness");
    const char* result_key = volume ? "volume" : "brightness";
    int64_t requested = 0;
    if (!OnlyKeys(arguments, {field}) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(arguments, field), adjust ? -100 : 0, 100,
                 requested)) {
        return Answer(Snapshot(execution_id, "rejected", 0, "invalid_arguments"));
    }

    const auto& read = volume ? actuators_.read_volume : actuators_.read_brightness;
    const auto& apply = volume ? actuators_.apply_volume : actuators_.apply_brightness;
    std::optional<int> applied;
    if (adjust) {
        const std::optional<int> current = read();
        if (current.has_value()) {
            applied = apply(std::clamp<int>(*current + static_cast<int>(requested), 0, 100));
        }
    } else {
        applied = apply(static_cast<int>(requested));
    }
    std::string snapshot = applied.has_value()
        ? Snapshot(execution_id, "completed", 1, "", result_key, *applied)
        : Snapshot(execution_id, "failed", 1, "hardware_error");
    if (snapshot.empty()) return Answer({});
    Remember({execution_id, request, snapshot});
    return Answer(std::move(snapshot));
}

RpcAnswer Runtime::Status(Caller caller, const std::string& payload, const Binding& binding)
{
    Json root(cJSON_ParseWithLength(payload.data(), payload.size()));
    std::string execution_id;
    std::string session_id;
    int64_t revision = 0;
    if (!root || !OnlyKeys(root.get(), {"execution_id", "session_id", "policy_revision"}) ||
        !Identifier(cJSON_GetObjectItemCaseSensitive(root.get(), "execution_id"), execution_id) ||
        !Identifier(cJSON_GetObjectItemCaseSensitive(root.get(), "session_id"), session_id) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(root.get(), "policy_revision"), 1, 4294967295.0,
                 revision)) {
        return Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request");
    }
    switch (CheckScope(caller, session_id, revision, binding)) {
    case Scope::Unauthorized: return Refuse(kRpcUnauthorized, "unauthorized");
    case Scope::Inactive: return Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request");
    case Scope::Bound: break;
    }
    const Entry* entry = Find(execution_id);
    if (entry == nullptr) return Refuse(kRpcInvalidOrInactive, "unknown_execution");
    return Answer(entry->snapshot);
}

RpcAnswer Runtime::ReadProperties(Caller caller) const
{
    if (caller == Caller::Unknown) return Refuse(kRpcUnauthorized, "unauthorized");
    Json root(cJSON_CreateObject());
    cJSON* values = root ? cJSON_AddObjectToObject(root.get(), "values") : nullptr;
    if (values == nullptr) return Answer({});
    if (HasSpeaker()) {
        if (const std::optional<int> volume = actuators_.read_volume(); volume.has_value()) {
            cJSON_AddNumberToObject(values, kVolumeProperty, *volume);
        }
    }
    if (HasDisplay()) {
        if (const std::optional<int> brightness = actuators_.read_brightness(); brightness.has_value()) {
            cJSON_AddNumberToObject(values, kBrightnessProperty, *brightness);
        }
    }
    return Answer(Print(root.get()));
}

std::optional<std::string> Runtime::PropertyChangedPayload(const std::string& property,
                                                           int value) const
{
    const bool declared = (property == kVolumeProperty && HasSpeaker()) ||
                          (property == kBrightnessProperty && HasDisplay());
    if (!declared || value < 0 || value > 100) return std::nullopt;
    Json root(cJSON_CreateObject());
    if (!root) return std::nullopt;
    cJSON_AddStringToObject(root.get(), "name", property.c_str());
    cJSON_AddNumberToObject(root.get(), "value", value);
    std::string text = Print(root.get());
    if (text.empty()) return std::nullopt;
    return text;
}

const Runtime::Entry* Runtime::Find(const std::string& execution_id) const
{
    for (const Entry& entry : ledger_) {
        if (entry.execution_id == execution_id) return &entry;
    }
    return nullptr;
}

void Runtime::Remember(Entry entry)
{
    if (ledger_.size() < kLedgerSize) {
        ledger_.push_back(std::move(entry));
        return;
    }
    ledger_[next_] = std::move(entry);
    next_ = (next_ + 1) % kLedgerSize;
}

}  // namespace eidolon::device_settings
