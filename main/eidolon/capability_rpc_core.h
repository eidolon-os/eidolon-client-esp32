#pragma once

// The device side of eidolon.capability.invoke.v1 and its status/cancel, shared
// by every capability this firmware executes (device_settings_core,
// camera_capture_core). The wire is the SDK's (biz/presentation/capability_rpc):
// an invocation carries its execution scope, and every answer about an
// execution is an ExecutionSnapshot. What a capability does lives with it; who
// may call, what binds a call and how a snapshot reads lives here, once.
//
// No hardware, no transport, no FreeRTOS: the host tests hold all of it.

#include <cstdint>
#include <string>
#include <vector>

// The method names come with the SDK's embedded declarations.
#include "eidolon/generated/device_settings_catalog.h"

namespace eidolon::capability_rpc {

// The four methods a device serves for its capabilities (SDK capability_rpc,
// device_properties).
inline constexpr const char* kInvokeMethod = device_settings::kInvokeMethod;
inline constexpr const char* kStatusMethod = device_settings::kStatusMethod;
inline constexpr const char* kCancelMethod = device_settings::kCancelMethod;
inline constexpr const char* kPropertiesReadMethod = device_settings::kPropertiesReadMethod;

// Who sent an invocation, classified by the transport the same way it
// classifies control packets.
enum class Caller { Unknown, Agent, Provider };

// What an invocation may be bound to right now. Gathered by the controller on
// its own task, so the runtimes never read controller state.
struct Binding {
    std::string room;                      // the standing channel's room
    uint32_t standing_policy_revision = 0; // 0: no Owner policy known
    std::string session_id;                // empty: no conversation
    uint32_t session_policy_revision = 0;
    std::vector<std::string> session_capabilities;
};

// An RPC answer. A refusal of the request itself (unauthorized, inactive
// binding, malformed) is an RPC error, as the Reachy runtime answers; a
// refusal of what was asked (unsupported, invalid arguments) is a snapshot.
struct RpcAnswer {
    bool ok = false;
    std::string payload;
    int error_code = 0;
    std::string error_message;
};

inline constexpr int kRpcUnauthorized = 2001;
inline constexpr int kRpcInvalidOrInactive = 2002;
inline constexpr size_t kMaxRpcPayloadBytes = 4096;

// One invocation, checked for shape: every field the SDK sends and nothing
// else, identifiers in the SDK's alphabet, versions this device speaks.
struct Invocation {
    std::string execution_id;
    std::string session_id;
    int64_t policy_revision = 0;
    std::string capability;
    std::string arguments;   // the arguments object, compact JSON
    int64_t expires_at_ms = 0;
    int64_t max_duration_ms = 0;
};
bool ParseInvocation(const std::string& payload, Invocation& out);

// The scope a status or cancel request names.
struct ExecutionScope {
    std::string execution_id;
    std::string session_id;
    int64_t policy_revision = 0;
};
bool ParseExecutionScope(const std::string& payload, ExecutionScope& out);

enum class Scope { Bound, Unauthorized, Inactive };
// A Provider call is bound to the standing channel (room + Owner policy
// revision), an Agent call to the conversation it serves.
Scope CheckScope(Caller caller, const std::string& session_id, int64_t revision,
                 const Binding& binding);
// The RPC error for a scope that is not Bound.
RpcAnswer RefuseScope(Scope scope);

RpcAnswer Refuse(int code, const char* message);
// A snapshot as the answer; an empty one (out of memory) is an RPC error.
RpcAnswer Answer(std::string payload);

// The request fields a replayed execution id is compared on.
std::string RequestKey(Caller caller, const Invocation& invocation);

// An ExecutionSnapshot in the SDK's field names. `result` is a JSON object
// text ("" for an empty one); `completion_basis` is omitted when null.
std::string Snapshot(const std::string& execution_id, const char* state, int sequence,
                     const char* error, const std::string& result = {},
                     const char* completion_basis = nullptr);

bool Contains(const std::vector<std::string>& values, const std::string& value);

}  // namespace eidolon::capability_rpc
