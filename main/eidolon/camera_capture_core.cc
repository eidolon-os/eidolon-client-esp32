#include "camera_capture_core.h"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace eidolon::camera_capture {
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

// An integer argument within the contract's bounds, or its default when absent.
bool Bounded(const cJSON* arguments, const char* name, int minimum, int maximum, int fallback,
             int& out)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(arguments, name);
    if (value == nullptr) {
        out = fallback;
        return true;
    }
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble)) return false;
    const double number = value->valuedouble;
    if (std::floor(number) != number || number < minimum || number > maximum) return false;
    out = static_cast<int>(number);
    return true;
}

// The arguments as the contract's input schema admits them: no other keys,
// purpose "look", edge and quality within bounds.
bool ParseArguments(const std::string& text, Request& request)
{
    Json arguments(cJSON_ParseWithLength(text.data(), text.size()));
    if (!cJSON_IsObject(arguments.get())) return false;
    for (const cJSON* item = arguments->child; item != nullptr; item = item->next) {
        if (item->string == nullptr) return false;
        const bool known = std::strcmp(item->string, "purpose") == 0 ||
                           std::strcmp(item->string, "max_edge") == 0 ||
                           std::strcmp(item->string, "quality") == 0;
        if (!known) return false;
        for (const cJSON* prior = arguments->child; prior != item; prior = prior->next) {
            if (std::strcmp(prior->string, item->string) == 0) return false;
        }
    }
    const cJSON* purpose = cJSON_GetObjectItemCaseSensitive(arguments.get(), "purpose");
    if (purpose != nullptr &&
        (!cJSON_IsString(purpose) || std::strcmp(purpose->valuestring, kPurposeLook) != 0)) {
        return false;
    }
    return Bounded(arguments.get(), "max_edge", kMaxEdgeMinimum, kMaxEdgeMaximum,
                   kMaxEdgeDefault, request.max_edge) &&
           Bounded(arguments.get(), "quality", kQualityMinimum, kQualityMaximum,
                   kQualityDefault, request.quality);
}

const char* StateName(int state)
{
    static constexpr const char* kNames[] = {"accepted", "running", "completed",
                                             "cancelled", "failed", "rejected"};
    return kNames[state];
}

bool InvocationOf(const std::string& payload, std::string& capability)
{
    Json root(cJSON_ParseWithLength(payload.data(), payload.size()));
    const cJSON* name = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "capability") : nullptr;
    if (!cJSON_IsString(name) || name->valuestring == nullptr) return false;
    capability = name->valuestring;
    return true;
}

bool ExecutionOf(const std::string& payload, std::string& execution_id)
{
    Json root(cJSON_ParseWithLength(payload.data(), payload.size()));
    const cJSON* id = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "execution_id") : nullptr;
    if (!cJSON_IsString(id) || id->valuestring == nullptr) return false;
    execution_id = id->valuestring;
    return true;
}

}  // namespace

std::string StillResult(const Still& still)
{
    Json root(cJSON_CreateObject());
    if (!root) return {};
    cJSON_AddStringToObject(root.get(), "media_type", kFrameMediaType);
    cJSON_AddNumberToObject(root.get(), "width", still.width);
    cJSON_AddNumberToObject(root.get(), "height", still.height);
    cJSON_AddNumberToObject(root.get(), "bytes", static_cast<double>(still.bytes));
    cJSON_AddStringToObject(root.get(), "sha256", still.sha256.c_str());
    // A device without a set clock cannot say when; the Host then measures the
    // age itself from when the frame arrived.
    if (still.captured_at_ms > 0) {
        cJSON_AddNumberToObject(root.get(), "captured_at_ms", static_cast<double>(still.captured_at_ms));
    }
    // The moment the frame was read: the sensor gives no timestamp of its own.
    cJSON_AddStringToObject(root.get(), "timestamp_kind", "read");
    return Print(root.get());
}

Runtime::Runtime(bool declared) : declared_(declared)
{
    ledger_.reserve(kLedgerSize);
}

bool Runtime::Handles(const std::string& method, const std::string& payload) const
{
    if (method == kInvokeMethod) {
        std::string capability;
        return InvocationOf(payload, capability) && capability == kCapability;
    }
    if (method == kStatusMethod || method == kCancelMethod) {
        std::string execution_id;
        return ExecutionOf(payload, execution_id) && Find(execution_id) != nullptr;
    }
    return false;
}

Outcome Runtime::Handle(const std::string& method, Caller caller, const std::string& payload,
                        const Binding& binding, bool capture_allowed, int64_t now_ms,
                        bool clock_set, uint64_t monotonic_ms)
{
    if (caller == Caller::Unknown) return {Refuse(kRpcUnauthorized, "unauthorized"), {}, {}};
    if (payload.size() > kMaxRpcPayloadBytes) return {Refuse(kRpcUnauthorized, "oversized"), {}, {}};
    if (method == kInvokeMethod) {
        return Invoke(caller, payload, binding, capture_allowed, now_ms, clock_set, monotonic_ms);
    }
    if (method == kStatusMethod || method == kCancelMethod) {
        return Follow(method, caller, payload, binding, monotonic_ms);
    }
    return {Refuse(kRpcInvalidOrInactive, "unsupported_method"), {}, {}};
}

Outcome Runtime::Invoke(Caller caller, const std::string& payload, const Binding& binding,
                        bool capture_allowed, int64_t now_ms, bool clock_set,
                        uint64_t monotonic_ms)
{
    Invocation invocation;
    if (!ParseInvocation(payload, invocation) || invocation.capability != kCapability) {
        return {Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request"), {}, {}};
    }
    const Scope scope = CheckScope(caller, invocation.session_id, invocation.policy_revision, binding);
    if (scope != Scope::Bound) return {RefuseScope(scope), {}, {}};

    const std::string& id = invocation.execution_id;
    const std::string key = RequestKey(caller, invocation);
    if (const Entry* previous = Find(id)) {
        // A retried invocation answers with where it already is; it never takes
        // a second still.
        if (previous->request_key != key) {
            return {Answer(Snapshot(id, "rejected", 0, "id_conflict")), {}, {}};
        }
        return {Answer(previous->snapshot), {}, {}};
    }
    const auto rejected = [&](const char* error) {
        Entry entry;
        entry.execution_id = id;
        entry.request_key = key;
        entry.state = State::Rejected;
        entry.snapshot = Snapshot(id, "rejected", 0, error);
        const std::string snapshot = entry.snapshot;
        Remember(std::move(entry));
        return Outcome{Answer(snapshot), {}, {}};
    };
    if (clock_set && (invocation.expires_at_ms <= now_ms ||
                      invocation.expires_at_ms > now_ms + 30000)) {
        return rejected("expired");
    }
    // A still is the agent's to ask for, inside the conversation whose plan
    // holds it; the Owner's app has no still to take on the standing channel.
    if (!declared_ || caller != Caller::Agent ||
        !Contains(binding.session_capabilities, kCapability)) {
        return rejected("unsupported_capability");
    }
    Request request;
    request.execution_id = id;
    request.session_id = invocation.session_id;
    request.policy_revision = static_cast<uint32_t>(invocation.policy_revision);
    if (!ParseArguments(invocation.arguments, request)) return rejected("invalid_arguments");

    Entry entry;
    entry.execution_id = id;
    entry.request_key = key;
    entry.request = request;
    if (!capture_allowed) {
        // The plan was made under a policy the Owner has since narrowed. Same
        // outcome as on the robot: the execution fails, it is not refused.
        entry.state = State::Failed;
        entry.sequence = 1;
        entry.snapshot = Snapshot(id, "failed", 1, "camera_disabled");
        const std::string snapshot = entry.snapshot;
        Remember(std::move(entry));
        return {Answer(snapshot), {}, {}};
    }
    if (!running_.empty() && !queued_.empty()) return rejected("busy");

    entry.deadline_ms = monotonic_ms +
        static_cast<uint64_t>(std::min<int64_t>(invocation.max_duration_ms, kTimeoutMs));
    entry.snapshot = Snapshot(id, "accepted", 0, "");
    const std::string accepted = entry.snapshot;
    Entry& kept = Remember(std::move(entry));
    Outcome outcome{Answer(accepted), {}, {}};
    if (running_.empty()) {
        running_ = id;
        kept.state = State::Running;
        kept.sequence = 1;
        kept.snapshot = Snapshot(id, "running", 1, "");
        outcome.start = request;
    } else {
        queued_ = id;
    }
    return outcome;
}

Outcome Runtime::Follow(const std::string& method, Caller caller, const std::string& payload,
                        const Binding& binding, uint64_t monotonic_ms)
{
    ExecutionScope request;
    if (!ParseExecutionScope(payload, request)) {
        return {Refuse(kRpcInvalidOrInactive, "invalid_or_inactive_request"), {}, {}};
    }
    const Scope scope = CheckScope(caller, request.session_id, request.policy_revision, binding);
    if (scope != Scope::Bound) return {RefuseScope(scope), {}, {}};
    Entry* entry = Find(request.execution_id);
    if (entry == nullptr) return {Refuse(kRpcInvalidOrInactive, "unknown_execution"), {}, {}};

    Outcome outcome;
    const bool in_flight = entry->state == State::Accepted || entry->state == State::Running;
    const bool held_camera = entry->execution_id == running_;
    if (in_flight && method == kCancelMethod) {
        outcome.start = Settle(*entry, State::Cancelled, "");
        if (held_camera) outcome.abort = request.execution_id;
    } else if (in_flight && monotonic_ms >= entry->deadline_ms) {
        // Whatever the camera is doing, the execution has an end.
        outcome.start = Settle(*entry, State::Failed, "deadline_exceeded");
        if (held_camera) outcome.abort = request.execution_id;
    }
    // Settle may have started the queued still; read the entry again.
    entry = Find(request.execution_id);
    outcome.answer = Answer(entry != nullptr ? entry->snapshot : std::string());
    return outcome;
}

bool Runtime::Wants(const std::string& execution_id) const
{
    const Entry* entry = Find(execution_id);
    return entry != nullptr && entry->state == State::Running && execution_id == running_;
}

bool Runtime::InFlight(const std::string& execution_id) const
{
    const Entry* entry = Find(execution_id);
    return entry != nullptr && (entry->state == State::Accepted || entry->state == State::Running);
}

std::optional<Request> Runtime::Completed(const std::string& execution_id, const Still& still)
{
    Entry* entry = Find(execution_id);
    if (entry == nullptr || entry->state != State::Running) return std::nullopt;
    const std::string result = StillResult(still);
    if (result.empty()) return Settle(*entry, State::Failed, "transfer_failed");
    return Settle(*entry, State::Completed, "", result);
}

std::optional<Request> Runtime::Failed(const std::string& execution_id, const char* error)
{
    Entry* entry = Find(execution_id);
    if (entry == nullptr || entry->state != State::Running) return std::nullopt;
    return Settle(*entry, State::Failed, error);
}

std::optional<std::string> Runtime::Close()
{
    std::optional<std::string> abort;
    if (!running_.empty()) abort = running_;
    for (Entry& entry : ledger_) {
        if (entry.state == State::Accepted || entry.state == State::Running) {
            entry.state = State::Cancelled;
            Rewrite(entry, "", {});
        }
    }
    running_.clear();
    queued_.clear();
    return abort;
}

std::optional<Request> Runtime::Settle(Entry& entry, State state, const char* error,
                                       const std::string& result)
{
    const bool held_camera = entry.execution_id == running_;
    if (entry.execution_id == queued_) queued_.clear();
    entry.state = state;
    Rewrite(entry, error, result);
    if (!held_camera) return std::nullopt;
    running_.clear();
    return StartQueued();
}

void Runtime::Rewrite(Entry& entry, const char* error, const std::string& result)
{
    entry.sequence += 1;
    entry.snapshot = Snapshot(entry.execution_id, StateName(static_cast<int>(entry.state)),
                              entry.sequence, error, result,
                              entry.state == State::Completed ? kCompletionBasis : nullptr);
}

std::optional<Request> Runtime::StartQueued()
{
    if (queued_.empty()) return std::nullopt;
    Entry* next = Find(queued_);
    queued_.clear();
    if (next == nullptr || next->state != State::Accepted) return std::nullopt;
    running_ = next->execution_id;
    next->state = State::Running;
    Rewrite(*next, "", {});
    return next->request;
}

Runtime::Entry* Runtime::Find(const std::string& execution_id)
{
    for (Entry& entry : ledger_) {
        if (entry.execution_id == execution_id) return &entry;
    }
    return nullptr;
}

const Runtime::Entry* Runtime::Find(const std::string& execution_id) const
{
    for (const Entry& entry : ledger_) {
        if (entry.execution_id == execution_id) return &entry;
    }
    return nullptr;
}

Runtime::Entry& Runtime::Remember(Entry entry)
{
    if (ledger_.size() < kLedgerSize) {
        ledger_.push_back(std::move(entry));
        return ledger_.back();
    }
    // Forget the oldest execution that has ended; one in flight is never
    // forgotten while the Channel may still ask about it.
    auto oldest = std::find_if(ledger_.begin(), ledger_.end(), [&](const Entry& kept) {
        return kept.execution_id != running_ && kept.execution_id != queued_;
    });
    ledger_.erase(oldest);
    ledger_.push_back(std::move(entry));
    return ledger_.back();
}

}  // namespace eidolon::camera_capture
