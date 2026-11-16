#pragma once

// camera.capture v1 on this device: one still on request, sent to the
// participant that asked for it. The contract is the Host's
// (eidolon_sdk.biz.capabilities.camera), embedded in generated/camera_catalog.h.
//
// This module owns what the execution means — who may ask, under which
// binding and Owner policy, with which arguments, and how its snapshots read
// from accepted through running to completed or failed — and nothing about
// how the still is taken or sent. The controller supplies those: it starts
// the camera when Handle() says so and reports back what happened. Taking a
// still outlives the RPC that asked for it, so the Channel follows it with
// status requests, exactly as it follows one on the Reachy robot.
//
// One still at a time, and at most one waiting behind it: a turn that asks who
// is here and what they are holding gets both, in order. Anything more is
// refused as busy.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "capability_rpc_core.h"
#include "eidolon/generated/camera_catalog.h"

namespace eidolon::camera_capture {

using capability_rpc::Binding;
using capability_rpc::Caller;
using capability_rpc::RpcAnswer;

// One still the camera is asked to take, for one execution.
struct Request {
    std::string execution_id;
    std::string session_id;
    uint32_t policy_revision = 0;
    int max_edge = kMaxEdgeDefault;
    int quality = kQualityDefault;
};

// What a still turned out to be. Its bytes stay with whoever sends them.
struct Still {
    int width = 0;
    int height = 0;
    size_t bytes = 0;
    std::string sha256;          // lowercase hex of exactly the bytes sent
    int64_t captured_at_ms = 0;  // wall clock when the frame was read; 0 when unknown
};

// What the controller has to do once a call is answered.
struct Outcome {
    RpcAnswer answer;
    std::optional<Request> start;       // take this still now
    std::optional<std::string> abort;   // stop the still of this execution
};

class Runtime {
public:
    // `declared`: this board declares camera.capture (it has a camera that
    // takes stills). An undeclared device refuses every invocation.
    explicit Runtime(bool declared);

    bool declared() const { return declared_; }

    // Whether a call is this module's: an invocation of camera.capture, or a
    // status or cancel request for an execution it holds.
    bool Handles(const std::string& method, const std::string& payload) const;

    // `capture_allowed`: the Owner's current policy lets a still leave the
    // device (inputs.camera == capture). `monotonic_ms` bounds how long an
    // execution may run, whatever happens to the camera.
    Outcome Handle(const std::string& method, Caller caller, const std::string& payload,
                   const Binding& binding, bool capture_allowed, int64_t now_ms, bool clock_set,
                   uint64_t monotonic_ms);

    // Whether the still for this execution is still wanted (not cancelled,
    // not timed out): checked before anything is sent.
    bool Wants(const std::string& execution_id) const;
    // Whether the execution is taking its still or waiting for the camera.
    bool InFlight(const std::string& execution_id) const;
    // The still was sent; the execution completes with its facts. Returns the
    // still waiting behind it, which the controller starts next.
    std::optional<Request> Completed(const std::string& execution_id, const Still& still);
    // The still could not be taken or sent; `error` is one of the contract's.
    std::optional<Request> Failed(const std::string& execution_id, const char* error);
    // The conversation the stills served is over: everything in flight is
    // cancelled. Returns the execution whose still should stop.
    std::optional<std::string> Close();

    // Exposed for tests: how many executions are remembered for replay/status.
    static constexpr size_t kLedgerSize = 8;

private:
    enum class State { Accepted, Running, Completed, Cancelled, Failed, Rejected };
    struct Entry {
        std::string execution_id;
        std::string request_key;
        State state = State::Accepted;
        int sequence = 0;
        std::string snapshot;
        uint64_t deadline_ms = 0;
        Request request;
    };

    Outcome Invoke(Caller caller, const std::string& payload, const Binding& binding,
                   bool capture_allowed, int64_t now_ms, bool clock_set, uint64_t monotonic_ms);
    Outcome Follow(const std::string& method, Caller caller, const std::string& payload,
                   const Binding& binding, uint64_t monotonic_ms);
    Entry* Find(const std::string& execution_id);
    const Entry* Find(const std::string& execution_id) const;
    Entry& Remember(Entry entry);
    // Moves an execution in flight to a terminal state; returns the queued
    // still that may start now, if this one held the camera.
    std::optional<Request> Settle(Entry& entry, State state, const char* error,
                                  const std::string& result = {});
    void Rewrite(Entry& entry, const char* error, const std::string& result);
    std::optional<Request> StartQueued();

    bool declared_ = false;
    std::vector<Entry> ledger_;
    std::string running_;   // execution holding the camera, "" when idle
    std::string queued_;    // execution waiting for it, "" when none
};

// The result object of a completed still, as the contract's result schema
// reads it.
std::string StillResult(const Still& still);

}  // namespace eidolon::camera_capture
