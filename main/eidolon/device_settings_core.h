#pragma once

// Device settings — volume and screen brightness — as installed capabilities.
//
// The contracts are the Host's (eidolon_sdk.biz.capabilities.device_settings),
// embedded from the SDK in generated/device_settings_catalog.h rather than
// retyped. This module is everything about them that does not touch hardware
// or the transport, so the host tests can hold all of it:
//
// - the Manifest declarations this board makes, exactly the generated ones;
// - one execution protocol for every caller (eidolon.capability.invoke.v1 and
//   its status/cancel), bound either to the conversation an agent serves or,
//   for the room's Provider, to the standing channel (the room and the Owner's
//   output policy revision);
// - an execution ledger, so a retried execution id answers with the outcome
//   already reached instead of acting twice;
// - observable property values, read on request and reported on change.
//
// The controller supplies the hardware as functions and the binding as a
// value, calls Handle() from its own task, and sends the answer.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace eidolon::device_settings {

// A value the device keeps, read and applied through the board. An unset
// function means the board does not have that hardware, and nothing about it
// is declared.
struct Actuators {
    std::function<std::optional<int>()> read_volume;
    std::function<std::optional<int>(int)> apply_volume;
    std::function<std::optional<int>()> read_brightness;
    std::function<std::optional<int>(int)> apply_brightness;
};

// Who sent an invocation, classified by the transport the same way it
// classifies control packets.
enum class Caller { Unknown, Agent, Provider };

// What an invocation may be bound to right now. Gathered by the controller on
// its own task, so this module never reads controller state.
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

class Runtime {
public:
    explicit Runtime(Actuators actuators);

    bool HasSpeaker() const;
    bool HasDisplay() const;

    // One RPC on one of the four methods this module serves. `clock_set` says
    // whether `now_ms` is wall-clock time a deadline can be compared with.
    RpcAnswer Handle(const std::string& method, Caller caller, const std::string& payload,
                     const Binding& binding, int64_t now_ms, bool clock_set);

    // The `property.changed` payload for a value that changed, or nothing for a
    // property this board does not declare or a value outside its schema.
    std::optional<std::string> PropertyChangedPayload(const std::string& property,
                                                      int value) const;

    static bool IsMethod(const std::string& method);

private:
    struct Entry {
        std::string execution_id;
        std::string request;  // scope + capability + arguments, compared on replay
        std::string snapshot;
    };

    RpcAnswer Invoke(Caller caller, const std::string& payload, const Binding& binding,
                     int64_t now_ms, bool clock_set);
    RpcAnswer Status(Caller caller, const std::string& payload, const Binding& binding);
    RpcAnswer ReadProperties(Caller caller) const;
    bool Declares(const std::string& capability) const;
    const Entry* Find(const std::string& execution_id) const;
    void Remember(Entry entry);

    Actuators actuators_;
    std::vector<Entry> ledger_;
    size_t next_ = 0;
};

// The Manifest declarations for the settings a board has, as comma-joined
// objects to splice into the actions / properties arrays; empty for none.
// Exactly the SDK's generated declarations, so the Host admits them.
std::string DeclaredActions(bool volume, bool brightness);
std::string DeclaredProperties(bool volume, bool brightness);

// Exposed for tests: the bound a ledger keeps before overwriting the oldest.
inline constexpr size_t kLedgerSize = 16;

}  // namespace eidolon::device_settings
