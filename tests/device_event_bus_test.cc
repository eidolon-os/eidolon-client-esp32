#include <cassert>
#include <string>

#include "device_event_builder.h"
#include "device_event_bus.h"
#include "eidolon_topics.h"

namespace {

std::string Event(const std::string& event_id = "evt-1",
                  const std::string& type = eidolon::kAmbientPresenceChangedType,
                  uint64_t occurred_at_ms = 1'700'000'000'000ULL,
                  uint64_t expires_at_ms = 1'700'000'003'000ULL,
                  const std::string& payload =
                      R"({"state":"present","modality":"mmwave","edge":"vacant_to_present"})")
{
    return R"({"schema_v":1,"kind":"event","event_id":")" + event_id +
           R"(","flow_id":"flow-1","causation_id":"","type":")" + type +
           R"(","source":{"device_id":"box3-1","component":"radar"},"occurred_at_ms":)" +
           std::to_string(occurred_at_ms) +
           R"(,"expires_at_ms":)" + std::to_string(expires_at_ms) +
           R"(,"payload":)" + payload + "}";
}

void TestValidEventDispatch()
{
    eidolon::DeviceEventBus bus;
    int calls = 0;
    assert(bus.RegisterHandler(
        eidolon::kAmbientPresenceChangedType,
        [&](const eidolon::DeviceEventMessage& event) {
            ++calls;
            assert(event.event_id == "evt-1");
            assert(event.flow_id == "flow-1");
            assert(event.causation_id.empty());
            assert(event.source_device_id == "box3-1");
            assert(event.source_component == "radar");
            assert(event.payload_json ==
                   R"({"state":"present","modality":"mmwave","edge":"vacant_to_present"})");
        }));

    assert(bus.Dispatch(Event(), 1'700'000'002'999ULL) ==
           eidolon::DeviceEventDispatchResult::Handled);
    assert(calls == 1);
}

void TestExpiryDuplicateAndUnknownType()
{
    eidolon::DeviceEventBus bus;
    assert(bus.Dispatch(Event("expired"), 1'700'000'003'000ULL) ==
           eidolon::DeviceEventDispatchResult::Expired);

    assert(bus.Dispatch(Event("unknown", "future.event")) ==
           eidolon::DeviceEventDispatchResult::Unhandled);
    assert(bus.Dispatch(Event("unknown", "future.event")) ==
           eidolon::DeviceEventDispatchResult::Duplicate);
}

void TestBoundedLruEviction()
{
    eidolon::DeviceEventBus bus(2);
    assert(bus.Dispatch(Event("evt-1")) ==
           eidolon::DeviceEventDispatchResult::Unhandled);
    assert(bus.Dispatch(Event("evt-2")) ==
           eidolon::DeviceEventDispatchResult::Unhandled);
    assert(bus.Dispatch(Event("evt-3")) ==
           eidolon::DeviceEventDispatchResult::Unhandled);
    assert(bus.Dispatch(Event("evt-1")) ==
           eidolon::DeviceEventDispatchResult::Unhandled);
}

void TestRejectsMalformedEnvelope()
{
    eidolon::DeviceEventBus bus;

    assert(bus.Dispatch("") == eidolon::DeviceEventDispatchResult::Invalid);
    assert(bus.Dispatch(Event() + "x") ==
           eidolon::DeviceEventDispatchResult::Invalid);
    assert(bus.Dispatch(Event("long-ttl", eidolon::kAmbientPresenceChangedType,
                              1'700'000'000'000ULL, 1'700'000'003'001ULL)) ==
           eidolon::DeviceEventDispatchResult::Invalid);
    assert(bus.Dispatch(
               Event("bad-payload", eidolon::kAmbientPresenceChangedType,
                     1'700'000'000'000ULL, 1'700'000'003'000ULL, "[]")) ==
           eidolon::DeviceEventDispatchResult::Invalid);

    std::string extra = Event("extra");
    extra.insert(extra.size() - 1, R"(,"target_device_id":"atk-1")");
    assert(bus.Dispatch(extra) == eidolon::DeviceEventDispatchResult::Invalid);

    std::string duplicate_key = Event("duplicate");
    duplicate_key.insert(1, R"("schema_v":1,)");
    assert(bus.Dispatch(duplicate_key) ==
           eidolon::DeviceEventDispatchResult::Invalid);

    std::string bad_component = Event("component");
    const std::string needle = R"("component":"radar")";
    bad_component.replace(bad_component.find(needle), needle.size(),
                          R"("component":"Radar")");
    assert(bus.Dispatch(bad_component) ==
           eidolon::DeviceEventDispatchResult::Invalid);
}

void TestHandlerReplacementAndLimit()
{
    eidolon::DeviceEventBus bus;
    int version = 0;
    assert(bus.RegisterHandler("replace.event",
                               [&](const auto&) { version = 1; }));
    assert(bus.RegisterHandler("replace.event",
                               [&](const auto&) { version = 2; }));
    assert(bus.Dispatch(Event("replace", "replace.event")) ==
           eidolon::DeviceEventDispatchResult::Handled);
    assert(version == 2);

    for (size_t index = 1; index < eidolon::DeviceEventBus::kMaxHandlers; ++index) {
        assert(bus.RegisterHandler("event." + std::to_string(index),
                                   [](const auto&) {}));
    }
    assert(!bus.RegisterHandler("event.overflow", [](const auto&) {}));
}

void TestBuildsDispatchableEvent()
{
    const std::string event_id =
        eidolon::MakeDeviceEventId("evt-owner", 1234, 0xabcdef01);
    assert(event_id == "evt-owner-1234-abcdef01");
    const std::string json = eidolon::BuildDeviceEventJson({
        .event_id = event_id,
        .flow_id = "flow-1",
        .causation_id = "evt-radar-1",
        .type = eidolon::kIdentityOwnerPresenceConfirmedType,
        .source_device_id = "atk-1",
        .source_component = "owner_face",
        .occurred_at_ms = 1'700'000'000'000ULL,
        .expires_at_ms = 1'700'000'003'000ULL,
        .payload_json =
            R"({"profile_revision":7,"guard_epoch":12,"presence_sequence":33,"evidence":"local_owner_face","raw_retention":"none"})",
    });
    assert(!json.empty());

    eidolon::DeviceEventBus bus;
    int calls = 0;
    assert(bus.RegisterHandler(
        eidolon::kIdentityOwnerPresenceConfirmedType,
        [&](const eidolon::DeviceEventMessage& event) {
            ++calls;
            assert(event.event_id == event_id);
            assert(event.causation_id == "evt-radar-1");
            assert(event.source_component == "owner_face");
        }));
    assert(bus.Dispatch(json) == eidolon::DeviceEventDispatchResult::Handled);
    assert(calls == 1);

    auto invalid = eidolon::DeviceEventEnvelope{
        .event_id = "evt",
        .flow_id = "flow",
        .type = "future.event",
        .source_device_id = "device",
        .source_component = "Bad Component",
        .occurred_at_ms = 1,
        .expires_at_ms = 2,
        .payload_json = "{}",
    };
    assert(eidolon::BuildDeviceEventJson(invalid).empty());
    invalid.source_component = "component";
    invalid.payload_json = "[]";
    assert(eidolon::BuildDeviceEventJson(invalid).empty());
}

}  // namespace

int main()
{
    TestValidEventDispatch();
    TestExpiryDuplicateAndUnknownType();
    TestBoundedLruEviction();
    TestRejectsMalformedEnvelope();
    TestHandlerReplacementAndLimit();
    TestBuildsDispatchableEvent();
    return 0;
}
