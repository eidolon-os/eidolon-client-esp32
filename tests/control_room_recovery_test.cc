#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "eidolon/control_room_recovery.h"

namespace {

struct RecoveryHarness {
    eidolon::ControlRoomRecovery recovery;
    bool switching_to_voice = false;
    bool has_control_config = true;
    bool control_generation = false;
    bool server_unreachable = false;
    bool rediscovered = false;
    int timers_scheduled = 0;
    int connect_calls = 0;
    uint32_t generation = 0;
    std::deque<bool> synchronous_connect_results;

    bool Schedule()
    {
        if (!recovery.TrySchedule(switching_to_voice, has_control_config)) {
            return false;
        }
        ++timers_scheduled;
        return true;
    }

    bool ConnectControl()
    {
        ++connect_calls;
        control_generation = true;
        ++generation;
        recovery.OnAttemptStarted();
        const bool ok = synchronous_connect_results.empty() ||
                        synchronous_connect_results.front();
        if (!synchronous_connect_results.empty()) {
            synchronous_connect_results.pop_front();
        }
        if (!ok) {
            recovery.OnDisconnected();
            ++generation;  // MarkSessionSuperseded("control_connect_sync_failed")
            Schedule();
        }
        return ok;
    }

    void OnControlTerminal(uint32_t event_generation)
    {
        if (event_generation != generation || !control_generation) {
            return;
        }
        recovery.OnDisconnected();
        Schedule();
    }

    void OnControlConnected(uint32_t event_generation)
    {
        if (event_generation != generation || !control_generation) {
            return;
        }
        recovery.OnConnected();
    }

    bool FireTimer()
    {
        int attempt = 0;
        if (!recovery.BeginRetry(&attempt)) {
            return false;
        }
        rediscovered = eidolon::ShouldRediscoverControlConfig(attempt);
        server_unreachable =
            server_unreachable || eidolon::ShouldSurfaceControlServerUnreachable(attempt);
        const bool ok = ConnectControl();
        recovery.FinishRetry();
        if (!ok) {
            Schedule();
        }
        return true;
    }
};

eidolon::RoomConfig Room(const char* token, const char* identity = "device")
{
    return {
        .server_url = "wss://livekit.test",
        .token = token,
        .identity = identity,
        .room_name = "device-control",
    };
}

void TestConnectedTerminalSchedulesAndRecovers()
{
    RecoveryHarness h;
    assert(h.ConnectControl());
    const uint32_t first_generation = h.generation;
    h.OnControlConnected(first_generation);
    assert(h.recovery.connected());

    h.OnControlTerminal(first_generation);
    assert(h.recovery.reconnect_pending());
    assert(h.timers_scheduled == 1);
    assert(h.FireTimer());
    const uint32_t recovered_generation = h.generation;
    h.OnControlConnected(recovered_generation);
    assert(h.recovery.connected());
    assert(h.recovery.reconnect_attempts() == 0);
    assert(!h.recovery.reconnect_pending());
}

void TestInitialSynchronousFailureRetries()
{
    RecoveryHarness h;
    h.synchronous_connect_results = {false, true};
    assert(!h.ConnectControl());
    assert(h.timers_scheduled == 1);
    assert(h.recovery.reconnect_pending());
    assert(h.FireTimer());
    h.OnControlConnected(h.generation);
    assert(h.recovery.connected());
    assert(h.connect_calls == 2);
}

void TestFailedAndDisconnectedDeduplicate()
{
    RecoveryHarness h;
    assert(h.ConnectControl());
    const uint32_t generation = h.generation;
    h.OnControlTerminal(generation);  // Failed
    h.OnControlTerminal(generation);  // trailing Disconnected
    assert(h.timers_scheduled == 1);
}

void TestSdkReconnectIsBoundedAndCanSelfHeal()
{
    RecoveryHarness h;
    assert(h.ConnectControl());
    const uint32_t generation = h.generation;
    h.recovery.OnReconnecting();
    assert(h.recovery.connect_in_flight());  // controller watchdog remains armed

    h.OnControlTerminal(generation);
    assert(h.recovery.reconnect_pending());
    h.OnControlConnected(generation);  // SDK recovered before our timer fired
    assert(h.recovery.connected());
    assert(!h.recovery.reconnect_pending());
    assert(!h.FireTimer());
}

void TestVoiceHandoffDropsOldGenerationAndQueuedTimer()
{
    RecoveryHarness h;
    assert(h.ConnectControl());
    const uint32_t control_generation = h.generation;
    h.OnControlTerminal(control_generation);
    assert(h.recovery.reconnect_pending());

    h.switching_to_voice = true;
    h.recovery.OnIntentionalTeardown();
    h.control_generation = false;
    ++h.generation;  // BeginSessionGeneration("voice")
    h.OnControlTerminal(control_generation);
    assert(h.timers_scheduled == 1);
    assert(!h.recovery.reconnect_pending());
    assert(!h.FireTimer());  // an already-queued timer event is harmless
}

void TestNetworkLostRestoredOwnsRecovery()
{
    RecoveryHarness h;
    assert(h.ConnectControl());
    h.OnControlTerminal(h.generation);
    assert(h.recovery.reconnect_pending());

    h.recovery.OnNetworkLost();
    h.control_generation = false;
    assert(!h.recovery.network_available());
    assert(!h.recovery.reconnect_pending());
    assert(!h.FireTimer());
    assert(!h.Schedule());

    h.recovery.OnNetworkRestored();
    assert(h.recovery.network_available());
    h.synchronous_connect_results = {true};
    assert(h.ConnectControl());
    h.OnControlConnected(h.generation);
    assert(h.recovery.connected());
}

void TestBackoffRediscoveryAndServerUnreachable()
{
    RecoveryHarness h;
    h.synchronous_connect_results = {false, false, false, false, true};
    assert(!h.ConnectControl());

    const std::vector<uint32_t> expected_delays = {1000, 2000, 4000, 8000, 16000,
                                                   30000, 30000};
    for (size_t i = 0; i < expected_delays.size(); ++i) {
        assert(eidolon::ControlReconnectDelayMs(static_cast<int>(i)) == expected_delays[i]);
    }

    assert(h.FireTimer());  // attempt 0: cached credentials, sync fail
    assert(!h.rediscovered);
    assert(!h.server_unreachable);
    assert(h.FireTimer());  // attempt 1: refresh credentials, sync fail
    assert(h.rediscovered);
    assert(!h.server_unreachable);
    assert(h.FireTimer());  // attempt 2: surface unreachable, sync fail
    assert(h.rediscovered);
    assert(h.server_unreachable);
    assert(h.FireTimer());  // attempt 3 succeeds, UI clears only on Connected
    assert(h.server_unreachable);
    h.OnControlConnected(h.generation);
    h.server_unreachable = false;
    assert(h.recovery.reconnect_attempts() == 0);
}

void TestRegistrationCredentialsWinAndGenerationNeverRollsBack()
{
    eidolon::Esp32HubConfig registration;
    registration.status = eidolon::HubConfigStatus::Active;
    registration.active = Room("voice-token", "device-identity");
    registration.control = Room("registration-token", "");
    registration.registration_id = "fresh-registration";
    const eidolon::RoomConfig stale_runtime = Room("stale-runtime-token", "guard-identity");

    auto selected = eidolon::BuildControlConnectionConfig(registration, &stale_runtime);
    assert(selected.active.token == "registration-token");
    assert(selected.active.identity == "device-identity");
    assert(selected.registration_id == "fresh-registration");

    registration.status = eidolon::HubConfigStatus::WaitingBinding;
    selected = eidolon::BuildControlConnectionConfig(registration, &stale_runtime);
    assert(selected.active.token == "stale-runtime-token");

    RecoveryHarness h;
    assert(h.ConnectControl());
    const uint32_t first = h.generation;
    h.OnControlTerminal(first);
    assert(h.FireTimer());
    assert(h.generation > first);
    const uint32_t refreshed = h.generation;
    h.OnControlTerminal(first);  // retired callback cannot revive old credentials
    assert(h.generation == refreshed);
    assert(!h.recovery.reconnect_pending());
}

}  // namespace

int main()
{
    TestConnectedTerminalSchedulesAndRecovers();
    TestInitialSynchronousFailureRetries();
    TestFailedAndDisconnectedDeduplicate();
    TestSdkReconnectIsBoundedAndCanSelfHeal();
    TestVoiceHandoffDropsOldGenerationAndQueuedTimer();
    TestNetworkLostRestoredOwnsRecovery();
    TestBackoffRediscoveryAndServerUnreachable();
    TestRegistrationCredentialsWinAndGenerationNeverRollsBack();
    return 0;
}
