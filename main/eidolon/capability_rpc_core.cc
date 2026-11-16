#include "capability_rpc_core.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <utility>

namespace eidolon::capability_rpc {
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

const cJSON* Field(const cJSON* object, const char* name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

}  // namespace

bool ParseInvocation(const std::string& payload, Invocation& out)
{
    Json root(cJSON_ParseWithLength(payload.data(), payload.size()));
    if (!root || !OnlyKeys(root.get(), {"execution_id", "session_id", "policy_revision",
                                        "protocol_version", "capability", "capability_version",
                                        "arguments", "expires_at_ms", "max_duration_ms"})) {
        return false;
    }
    Invocation invocation;
    int64_t protocol = 0;
    int64_t version = 0;
    const cJSON* arguments = Field(root.get(), "arguments");
    const cJSON* name = Field(root.get(), "capability");
    if (!Identifier(Field(root.get(), "execution_id"), invocation.execution_id) ||
        !Identifier(Field(root.get(), "session_id"), invocation.session_id) ||
        !Integer(Field(root.get(), "policy_revision"), 1, 4294967295.0,
                 invocation.policy_revision) ||
        !Integer(Field(root.get(), "protocol_version"), 1, 1, protocol) ||
        !Integer(Field(root.get(), "capability_version"), 1, 1, version) ||
        !Integer(Field(root.get(), "expires_at_ms"), 1, 9007199254740991.0,
                 invocation.expires_at_ms) ||
        !Integer(Field(root.get(), "max_duration_ms"), 1, 30000, invocation.max_duration_ms) ||
        !cJSON_IsString(name) || name->valuestring == nullptr ||
        std::strlen(name->valuestring) > 128 || !cJSON_IsObject(arguments)) {
        return false;
    }
    invocation.capability = name->valuestring;
    invocation.arguments = Print(arguments);
    if (invocation.arguments.empty()) return false;
    out = std::move(invocation);
    return true;
}

bool ParseExecutionScope(const std::string& payload, ExecutionScope& out)
{
    Json root(cJSON_ParseWithLength(payload.data(), payload.size()));
    ExecutionScope scope;
    if (!root || !OnlyKeys(root.get(), {"execution_id", "session_id", "policy_revision"}) ||
        !Identifier(Field(root.get(), "execution_id"), scope.execution_id) ||
        !Identifier(Field(root.get(), "session_id"), scope.session_id) ||
        !Integer(Field(root.get(), "policy_revision"), 1, 4294967295.0, scope.policy_revision)) {
        return false;
    }
    out = std::move(scope);
    return true;
}

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

RpcAnswer RefuseScope(Scope scope)
{
    return scope == Scope::Unauthorized ? Refuse(kRpcUnauthorized, "unauthorized")
                                        : Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request");
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

std::string RequestKey(Caller caller, const Invocation& invocation)
{
    return std::string(caller == Caller::Agent ? "agent" : "provider") + "|" +
           invocation.session_id + "|" + std::to_string(invocation.policy_revision) + "|" +
           invocation.capability + "|" + invocation.arguments;
}

std::string Snapshot(const std::string& execution_id, const char* state, int sequence,
                     const char* error, const std::string& result, const char* completion_basis)
{
    Json root(cJSON_CreateObject());
    if (!root) return {};
    cJSON_AddStringToObject(root.get(), "execution_id", execution_id.c_str());
    cJSON_AddStringToObject(root.get(), "state", state);
    cJSON_AddNumberToObject(root.get(), "sequence", sequence);
    cJSON* values = result.empty() ? cJSON_CreateObject()
                                   : cJSON_ParseWithLength(result.data(), result.size());
    if (!cJSON_IsObject(values)) {
        cJSON_Delete(values);
        return {};
    }
    cJSON_AddItemToObject(root.get(), "result", values);
    if (completion_basis != nullptr) {
        cJSON_AddStringToObject(root.get(), "completion_basis", completion_basis);
    }
    cJSON_AddStringToObject(root.get(), "error", error);
    return Print(root.get());
}

bool Contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace eidolon::capability_rpc
