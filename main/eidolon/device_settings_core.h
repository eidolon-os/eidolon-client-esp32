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

#include "capability_rpc_core.h"

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

// The invoke.v1 protocol pieces this module shares with every other device
// capability (capability_rpc_core): who called, what a call is bound to, how
// it is answered.
using capability_rpc::Binding;
using capability_rpc::Caller;
using capability_rpc::RpcAnswer;
using capability_rpc::kMaxRpcPayloadBytes;
using capability_rpc::kRpcInvalidOrInactive;
using capability_rpc::kRpcUnauthorized;

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
