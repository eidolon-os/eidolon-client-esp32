// camera.capture v1 as the Channel drives it: invoke, then status until the
// execution ends, with the still sent before completion is reported.
#include "eidolon/camera_capture_core.h"

#include <cJSON.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace eidolon;
using namespace eidolon::camera_capture;
using capability_rpc::kCancelMethod;
using capability_rpc::kInvokeMethod;
using capability_rpc::kRpcInvalidOrInactive;
using capability_rpc::kRpcUnauthorized;
using capability_rpc::kStatusMethod;

namespace {

constexpr int64_t kNow = 1763280000000;

Binding InSession(bool with_camera = true)
{
    Binding binding;
    binding.room = "device-room";
    binding.standing_policy_revision = 7;
    binding.session_id = "conv-1";
    binding.session_policy_revision = 7;
    if (with_camera) binding.session_capabilities = {"audio.volume.set", kCapability};
    return binding;
}

std::string Invoke(const std::string& id, const std::string& arguments = "{}",
                   const char* session = "conv-1", int64_t expires = kNow + 10000,
                   const char* capability = kCapability)
{
    return std::string("{\"execution_id\":\"") + id + "\",\"session_id\":\"" + session +
           "\",\"policy_revision\":7,\"protocol_version\":1,\"capability\":\"" + capability +
           "\",\"capability_version\":1,\"arguments\":" + arguments +
           ",\"expires_at_ms\":" + std::to_string(expires) + ",\"max_duration_ms\":30000}";
}

std::string Scope(const std::string& id, const char* session = "conv-1")
{
    return std::string("{\"execution_id\":\"") + id + "\",\"session_id\":\"" + session +
           "\",\"policy_revision\":7}";
}

struct Snap {
    std::string state;
    int sequence = -1;
    std::string error;
    std::string basis;
    cJSON* result = nullptr;
    cJSON* root = nullptr;
    ~Snap() { cJSON_Delete(root); }
};

void Read(const RpcAnswer& answer, Snap& out)
{
    assert(answer.ok);
    out.root = cJSON_Parse(answer.payload.c_str());
    assert(out.root != nullptr);
    out.state = cJSON_GetObjectItem(out.root, "state")->valuestring;
    out.sequence = cJSON_GetObjectItem(out.root, "sequence")->valueint;
    out.error = cJSON_GetObjectItem(out.root, "error")->valuestring;
    const cJSON* basis = cJSON_GetObjectItem(out.root, "completion_basis");
    out.basis = basis ? basis->valuestring : "";
    out.result = cJSON_GetObjectItem(out.root, "result");
    assert(cJSON_IsObject(out.result));
}

Outcome Call(Runtime& runtime, const char* method, const std::string& payload,
             bool allowed = true, uint64_t monotonic = 1000, const Binding& binding = InSession(),
             Caller caller = Caller::Agent)
{
    return runtime.Handle(method, caller, payload, binding, allowed, kNow, true, monotonic);
}

Still Taken()
{
    Still still;
    still.width = 640;
    still.height = 480;
    still.bytes = 41234;
    still.sha256 = std::string(64, 'a');
    still.captured_at_ms = kNow + 300;
    return still;
}

void ExpectState(Runtime& runtime, const std::string& id, const char* state, int sequence,
                 const char* error = "")
{
    Snap snap;
    Read(Call(runtime, kStatusMethod, Scope(id)).answer, snap);
    assert(snap.state == state && snap.sequence == sequence && snap.error == error);
}

void TestStillCompletesWithItsFacts()
{
    Runtime runtime(true);
    assert(runtime.Handles(kInvokeMethod, Invoke("e1")));
    assert(!runtime.Handles(kInvokeMethod, Invoke("e1", "{\"volume\":3}", "conv-1", kNow + 10000,
                                                 "audio.volume.set")));
    assert(!runtime.Handles(kStatusMethod, Scope("e1")));   // nothing held yet

    const Outcome invoked = Call(runtime, kInvokeMethod, Invoke("e1", "{\"purpose\":\"look\",\"max_edge\":1280}"));
    Snap accepted;
    Read(invoked.answer, accepted);
    assert(accepted.state == "accepted" && accepted.sequence == 0);
    assert(invoked.start && invoked.start->execution_id == "e1");
    assert(invoked.start->max_edge == 1280 && invoked.start->quality == kQualityDefault);
    assert(invoked.start->session_id == "conv-1" && invoked.start->policy_revision == 7);
    assert(runtime.Handles(kStatusMethod, Scope("e1")) && runtime.Handles(kCancelMethod, Scope("e1")));
    ExpectState(runtime, "e1", "running", 1);
    assert(runtime.Wants("e1"));

    assert(!runtime.Completed("e1", Taken()));   // nothing was waiting behind it
    assert(!runtime.Wants("e1"));
    Snap done;
    Read(Call(runtime, kStatusMethod, Scope("e1")).answer, done);
    assert(done.state == "completed" && done.sequence == 2 && done.error.empty());
    assert(done.basis == kCompletionBasis);
    // Every result key is one the contract's result schema names.
    cJSON* action = cJSON_Parse(kAction);
    const cJSON* allowed = cJSON_GetObjectItem(cJSON_GetObjectItem(action, "output_schema"), "properties");
    for (const cJSON* item = done.result->child; item != nullptr; item = item->next) {
        assert(cJSON_GetObjectItem(allowed, item->string) != nullptr);
    }
    cJSON_Delete(action);
    assert(std::string(cJSON_GetObjectItem(done.result, "media_type")->valuestring) == "image/jpeg");
    assert(cJSON_GetObjectItem(done.result, "width")->valueint == 640);
    assert(cJSON_GetObjectItem(done.result, "bytes")->valueint == 41234);
    assert(std::string(cJSON_GetObjectItem(done.result, "sha256")->valuestring) == std::string(64, 'a'));
    assert(cJSON_GetObjectItem(done.result, "captured_at_ms")->valuedouble == double(kNow + 300));
    assert(std::string(cJSON_GetObjectItem(done.result, "timestamp_kind")->valuestring) == "read");

    // A retried invocation answers where it is and never takes a second still.
    const Outcome replay = Call(runtime, kInvokeMethod, Invoke("e1", "{\"purpose\":\"look\",\"max_edge\":1280}"));
    Snap again;
    Read(replay.answer, again);
    assert(!replay.start && again.state == "completed" && again.sequence == 2);
    Snap conflict;
    Read(Call(runtime, kInvokeMethod, Invoke("e1", "{\"max_edge\":256}")).answer, conflict);
    assert(conflict.state == "rejected" && conflict.error == "id_conflict");

    // A clock the device does not trust: no capture time, and no expiry check.
    Still unclocked = Taken();
    unclocked.captured_at_ms = 0;
    cJSON* result = cJSON_Parse(StillResult(unclocked).c_str());
    assert(cJSON_GetObjectItem(result, "captured_at_ms") == nullptr);
    cJSON_Delete(result);
}

void TestOnlyThePlannedAgentMayAsk()
{
    Runtime runtime(true);
    const auto refused = [&](const Outcome& outcome, const char* error) {
        Snap snap;
        Read(outcome.answer, snap);
        assert(!outcome.start && snap.state == "rejected" && snap.error == error);
    };
    refused(Call(runtime, kInvokeMethod, Invoke("n1"), true, 1000, InSession(false)), "unsupported_capability");
    Binding standing = InSession();
    refused(runtime.Handle(kInvokeMethod, Caller::Provider,
                           "{\"execution_id\":\"p1\",\"session_id\":\"device-room\",\"policy_revision\":7,"
                           "\"protocol_version\":1,\"capability\":\"camera.capture\",\"capability_version\":1,"
                           "\"arguments\":{},\"expires_at_ms\":1763280010000,\"max_duration_ms\":30000}",
                           standing, true, kNow, true, 1000),
            "unsupported_capability");
    Runtime undeclared(false);
    refused(Call(undeclared, kInvokeMethod, Invoke("u1")), "unsupported_capability");

    // Outside the bound conversation the request itself is refused.
    const Outcome other = Call(runtime, kInvokeMethod, Invoke("o1", "{}", "conv-2"));
    assert(!other.answer.ok && other.answer.error_code == kRpcInvalidOrInactive && !other.start);
    const Outcome stranger = Call(runtime, kInvokeMethod, Invoke("s1"), true, 1000, InSession(), Caller::Unknown);
    assert(!stranger.answer.ok && stranger.answer.error_code == kRpcUnauthorized);
    Binding closed = InSession();
    closed.session_id.clear();
    const Outcome after = Call(runtime, kInvokeMethod, Invoke("c1"), true, 1000, closed);
    assert(!after.answer.ok && after.answer.error_code == kRpcInvalidOrInactive);

    refused(runtime.Handle(kInvokeMethod, Caller::Agent, Invoke("x1", "{}", "conv-1", kNow - 1),
                           InSession(), true, kNow, true, 1000),
            "expired");
    // Without a trusted clock the expiry cannot be judged and is not.
    const Outcome unclocked = runtime.Handle(kInvokeMethod, Caller::Agent, Invoke("x2", "{}", "conv-1", 5),
                                             InSession(), true, 0, false, 1000);
    assert(unclocked.start);
}

void TestArgumentsAreTheContracts()
{
    Runtime runtime(true);
    for (const char* arguments : {"{\"purpose\":\"read\"}", "{\"max_edge\":255}", "{\"max_edge\":1281}",
                                  "{\"quality\":96}", "{\"quality\":49}", "{\"max_edge\":640.5}",
                                  "{\"max_edge\":\"640\"}", "{\"zoom\":2}",
                                  "{\"quality\":80,\"quality\":81}"}) {
        static int next = 0;
        const Outcome outcome = Call(runtime, kInvokeMethod, Invoke("a" + std::to_string(++next), arguments));
        Snap snap;
        Read(outcome.answer, snap);
        assert(!outcome.start && snap.state == "rejected" && snap.error == "invalid_arguments");
    }
    const Outcome bounds = Call(runtime, kInvokeMethod, Invoke("b1", "{\"max_edge\":256,\"quality\":95}"));
    assert(bounds.start && bounds.start->max_edge == 256 && bounds.start->quality == 95);
}

void TestNarrowedPolicyFailsTheStill()
{
    Runtime runtime(true);
    const Outcome outcome = Call(runtime, kInvokeMethod, Invoke("d1"), /*allowed=*/false);
    Snap snap;
    Read(outcome.answer, snap);
    assert(!outcome.start && snap.state == "failed" && snap.error == "camera_disabled");
    ExpectState(runtime, "d1", "failed", 1, "camera_disabled");
    assert(!runtime.Wants("d1"));
}

void TestOneStillAtATimeOneWaiting()
{
    Runtime runtime(true);
    const Outcome first = Call(runtime, kInvokeMethod, Invoke("q1"));
    assert(first.start && first.start->execution_id == "q1");
    const Outcome second = Call(runtime, kInvokeMethod, Invoke("q2", "{\"max_edge\":512}"));
    Snap waiting;
    Read(second.answer, waiting);
    assert(!second.start && waiting.state == "accepted");
    ExpectState(runtime, "q2", "accepted", 0);
    Snap busy;
    Read(Call(runtime, kInvokeMethod, Invoke("q3")).answer, busy);
    assert(busy.state == "rejected" && busy.error == "busy");
    assert(!runtime.Wants("q2"));
    // Only these two have a still coming, so only they need a way back.
    assert(runtime.InFlight("q1") && runtime.InFlight("q2") && !runtime.InFlight("q3"));

    const auto next = runtime.Completed("q1", Taken());
    assert(next && next->execution_id == "q2" && next->max_edge == 512);
    ExpectState(runtime, "q2", "running", 1);
    assert(runtime.Wants("q2"));
    assert(!runtime.Failed("q2", "no_frame"));
    ExpectState(runtime, "q2", "failed", 2, "no_frame");
    // The camera is free again.
    assert(Call(runtime, kInvokeMethod, Invoke("q4")).start);
}

void TestCancelAndDeadline()
{
    Runtime runtime(true);
    assert(Call(runtime, kInvokeMethod, Invoke("c1")).start);
    assert(!Call(runtime, kInvokeMethod, Invoke("c2")).start);
    const Outcome cancelled = Call(runtime, kCancelMethod, Scope("c1"));
    Snap snap;
    Read(cancelled.answer, snap);
    assert(snap.state == "cancelled" && snap.sequence == 2);
    assert(cancelled.abort && *cancelled.abort == "c1");
    assert(cancelled.start && cancelled.start->execution_id == "c2");
    // A still that arrives after its execution was cancelled is not wanted
    // and changes nothing.
    assert(!runtime.Wants("c1"));
    assert(!runtime.Completed("c1", Taken()));
    ExpectState(runtime, "c1", "cancelled", 2);

    // Cancelling one that is waiting only takes it out of the queue.
    assert(!Call(runtime, kInvokeMethod, Invoke("c3")).start);
    const Outcome dequeued = Call(runtime, kCancelMethod, Scope("c3"));
    assert(!dequeued.abort && !dequeued.start);
    ExpectState(runtime, "c3", "cancelled", 1);

    // c2 started at 1000 with a 10 s bound (the contract's timeout, below the
    // invocation's 30 s): the status that finds it late ends it.
    const Outcome early = Call(runtime, kStatusMethod, Scope("c2"), true, 10999);
    Snap running;
    Read(early.answer, running);
    assert(running.state == "running" && !early.abort);
    const Outcome late = Call(runtime, kStatusMethod, Scope("c2"), true, 11000);
    Snap timed_out;
    Read(late.answer, timed_out);
    assert(timed_out.state == "failed" && timed_out.error == "deadline_exceeded");
    assert(late.abort && *late.abort == "c2");
    // Ending is final: a second cancel answers the outcome reached.
    const Outcome again = Call(runtime, kCancelMethod, Scope("c2"));
    Snap still_failed;
    Read(again.answer, still_failed);
    assert(still_failed.state == "failed" && !again.abort);
}

void TestCloseCancelsEverythingInFlight()
{
    Runtime runtime(true);
    assert(Call(runtime, kInvokeMethod, Invoke("z1")).start);
    assert(!Call(runtime, kInvokeMethod, Invoke("z2")).start);
    const auto abort = runtime.Close();
    assert(abort && *abort == "z1");
    assert(!runtime.Wants("z1") && !runtime.Wants("z2"));
    ExpectState(runtime, "z1", "cancelled", 2);
    ExpectState(runtime, "z2", "cancelled", 1);
    assert(!runtime.Close());
    assert(Call(runtime, kInvokeMethod, Invoke("z3")).start);
}

void TestLedgerIsBounded()
{
    Runtime runtime(true);
    for (int i = 0; i < 12; ++i) {
        const std::string id = "l" + std::to_string(i);
        assert(Call(runtime, kInvokeMethod, Invoke(id)).start);
        assert(!runtime.Completed(id, Taken()));
    }
    const Outcome forgotten = Call(runtime, kStatusMethod, Scope("l0"));
    assert(!forgotten.answer.ok && forgotten.answer.error_code == kRpcInvalidOrInactive);
    assert(!runtime.Handles(kStatusMethod, Scope("l0")));
    ExpectState(runtime, "l11", "completed", 2);
    // An execution in flight survives however many end around it.
    assert(Call(runtime, kInvokeMethod, Invoke("keep")).start);
    for (int i = 0; i < 12; ++i) {
        Snap snap;
        Read(Call(runtime, kInvokeMethod, Invoke("r" + std::to_string(i), "{\"zoom\":1}")).answer, snap);
    }
    assert(runtime.Wants("keep"));
    ExpectState(runtime, "keep", "running", 1);
}

}  // namespace

int main()
{
    TestStillCompletesWithItsFacts();
    TestOnlyThePlannedAgentMayAsk();
    TestArgumentsAreTheContracts();
    TestNarrowedPolicyFailsTheStill();
    TestOneStillAtATimeOneWaiting();
    TestCancelAndDeadline();
    TestCloseCancelsEverythingInFlight();
    TestLedgerIsBounded();
    std::puts("camera capture core tests passed");
    return 0;
}
