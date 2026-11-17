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

using namespace capability_rpc;

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

bool Integer(const cJSON* value, double minimum, double maximum, int64_t& out)
{
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble)) return false;
    const double number = value->valuedouble;
    if (std::floor(number) != number || number < minimum || number > maximum) return false;
    out = static_cast<int64_t>(number);
    return true;
}

bool OwnerControlled(const std::string& capability)
{
    return std::any_of(kOwnerControl.begin(), kOwnerControl.end(),
                       [&](std::string_view name) { return capability == name; });
}

// A setting's snapshot: its one value as the result, and done when the
// hardware holds it, which is what the registry promises for these contracts.
std::string SettingSnapshot(const std::string& execution_id, const char* state, int sequence,
                            const char* error, const char* result_key = nullptr, int result = 0)
{
    const std::string values = result_key == nullptr
        ? std::string()
        : std::string("{\"") + result_key + "\":" + std::to_string(result) + "}";
    return capability_rpc::Snapshot(execution_id, state, sequence, error, values,
                                    std::strcmp(state, "completed") == 0 ? "configuration_applied"
                                                                         : nullptr);
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
    Invocation invocation;
    if (!ParseInvocation(payload, invocation)) {
        return Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request");
    }
    const Scope scope = CheckScope(caller, invocation.session_id, invocation.policy_revision, binding);
    if (scope != Scope::Bound) return RefuseScope(scope);

    const std::string& execution_id = invocation.execution_id;
    const std::string& capability = invocation.capability;
    const std::string request = RequestKey(caller, invocation);
    if (const Entry* previous = Find(execution_id)) {
        if (previous->request != request) {
            return Answer(SettingSnapshot(execution_id, "rejected", 0, "id_conflict"));
        }
        return Answer(previous->snapshot);
    }
    if (clock_set && (invocation.expires_at_ms <= now_ms || invocation.expires_at_ms > now_ms + 30000)) {
        return Answer(SettingSnapshot(execution_id, "rejected", 0, "expired"));
    }
    const bool allowed = Declares(capability) &&
        (caller == Caller::Provider ? OwnerControlled(capability)
                                    : Contains(binding.session_capabilities, capability));
    if (!allowed) return Answer(SettingSnapshot(execution_id, "rejected", 0, "unsupported_capability"));

    Json arguments(cJSON_ParseWithLength(invocation.arguments.data(), invocation.arguments.size()));
    const bool volume = capability == kVolumeSet || capability == kVolumeAdjust;
    const bool adjust = capability == kVolumeAdjust || capability == kBrightnessAdjust;
    const char* field = adjust ? "change" : (volume ? "volume" : "brightness");
    const char* result_key = volume ? "volume" : "brightness";
    int64_t requested = 0;
    if (!OnlyKeys(arguments.get(), {field}) ||
        !Integer(cJSON_GetObjectItemCaseSensitive(arguments.get(), field), adjust ? -100 : 0, 100,
                 requested)) {
        return Answer(SettingSnapshot(execution_id, "rejected", 0, "invalid_arguments"));
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
        ? SettingSnapshot(execution_id, "completed", 1, "", result_key, *applied)
        : SettingSnapshot(execution_id, "failed", 1, "hardware_error");
    if (snapshot.empty()) return Answer({});
    Remember({execution_id, request, snapshot});
    return Answer(std::move(snapshot));
}

RpcAnswer Runtime::Status(Caller caller, const std::string& payload, const Binding& binding)
{
    ExecutionScope request;
    if (!ParseExecutionScope(payload, request)) {
        return Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request");
    }
    const Scope scope = CheckScope(caller, request.session_id, request.policy_revision, binding);
    if (scope != Scope::Bound) return RefuseScope(scope);
    const Entry* entry = Find(request.execution_id);
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

bool ShouldConfirmVolume(const VolumeCueContext& context)
{
    return context.volume > 0 && !context.in_conversation && !context.playback_active &&
           context.audio_cue_allowed;
}

}  // namespace eidolon::device_settings
