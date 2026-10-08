#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "eidolon/channel_recovery.h"

namespace {

struct RecoveryHarness {
    eidolon::ChannelRecovery recovery;
    bool switching_to_voice = false;
    bool has_channel_config = true;
    bool control_generation = false;
    bool server_unreachable = false;
    bool rediscovered = false;
    int timers_scheduled = 0;
    int connect_calls = 0;
    uint32_t generation = 0;
    std::deque<bool> synchronous_connect_results;

    bool Schedule()
    {
        if (!recovery.TrySchedule(switching_to_voice, has_channel_config)) {
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
        rediscovered = eidolon::ShouldRediscoverChannelConfig(attempt);
        server_unreachable =
            server_unreachable || eidolon::ShouldSurfaceChannelServerUnreachable(attempt);
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

void TestStandingChannelOpenAdmission()
{
    using namespace eidolon;
    Esp32HubConfig config;
    config.status = HubConfigStatus::Active;
    config.session = Room("token");
    config.expires_at_ms = 1700000010000LL;
    // A persisted credential without a clock anchor must first recover.
    assert(!CanOpenOnStandingChannel(config, true, false, false, 1000));
    config.clock.Observe(1700000000000LL, 0, 0);
    assert(CanOpenOnStandingChannel(config, true, false, false, 1000));
    assert(!CanOpenOnStandingChannel(config, false, false, false, 1000));
    assert(!CanOpenOnStandingChannel(config, true, true, false, 1000));
    assert(!CanOpenOnStandingChannel(config, true, false, true, 1000));
    assert(!CanOpenOnStandingChannel(config, true, false, false, 10000));
    for (auto status : {HubConfigStatus::Revoked, HubConfigStatus::WaitingBinding,
                        HubConfigStatus::RecoveryRequired, HubConfigStatus::PendingApproval}) {
        config.status = status;
        assert(!CanOpenOnStandingChannel(config, true, false, false, 1000));
    }
}

void TestExpiredCredentialsStillRecoverAfterRefreshFailures()
{
    using namespace eidolon;
    Esp32HubConfig config;
    config.status = HubConfigStatus::Active;
    config.expires_at_ms = 1700000010000LL;
    config.clock.Observe(1700000000000LL, 0, 0);
    assert(!ChannelBindingExpired(config, 1000));
    assert(ChannelBindingExpired(config, 10000));
    ChannelRecovery recovery;
    recovery.OnConnected(0);
    recovery.OnDisconnected(10000);
    const auto allowed = [&] { return ChannelRecoveryAllowed(config.status, true, false); };
    assert(recovery.TrySchedule(false, allowed()));
    // The authority stays unreachable across repeated attempts; expiry cannot
    // stop admission, and each next attempt still refreshes before connecting.
    for (int failure = 0; failure < 12; ++failure) {
        int attempt = -1;
        assert(recovery.BeginRetry(&attempt));
        assert(ShouldRediscoverChannelConfig(attempt, false));
        recovery.FinishRetry();
        assert(recovery.TrySchedule(false, allowed()));
        assert(ChannelReconnectDelayMs(recovery.reconnect_attempts()) <= 30000);
    }
    int attempt = -1;
    assert(recovery.BeginRetry(&attempt));
    config.expires_at_ms = 1700000100000LL;
    assert(!ChannelBindingExpired(config, 20000));
    recovery.OnAttemptStarted();
    recovery.FinishRetry();
    recovery.OnConnected(20000);
    assert(recovery.connected());
    assert(!recovery.reconnect_pending());
    assert(ShouldRediscoverChannelConfig(0, false));
    assert(!ShouldRediscoverChannelConfig(0, true));
}

void TestRecoveryRespectsAuthorityAndNetworkBoundaries()
{
    using namespace eidolon;
    for (auto status : {HubConfigStatus::PendingApproval, HubConfigStatus::WaitingBinding,
                        HubConfigStatus::Revoked, HubConfigStatus::RecoveryRequired}) {
        assert(!ChannelRecoveryAllowed(status, true, false));
    }
    assert(!ChannelRecoveryAllowed(HubConfigStatus::Active, false, false));
    assert(!ChannelRecoveryAllowed(HubConfigStatus::Active, true, true));
    ChannelRecovery recovery;
    assert(recovery.TrySchedule(false, true));
    recovery.OnNetworkLost();
    int attempt = -1;
    assert(!recovery.BeginRetry(&attempt));
    assert(!recovery.TrySchedule(false, true));
    recovery.OnNetworkRestored();
    assert(recovery.TrySchedule(false, true));
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
    assert(h.recovery.reconnect_attempts() > 0);
    assert(!h.recovery.reconnect_pending());
}

void TestFlappingConnectionPreservesBackoffUntilStable()
{
    eidolon::ChannelRecovery recovery;
    recovery.OnConnected(1000);
    for (int i = 0; i < 8; ++i) {
        recovery.OnReconnecting(2000 + i * 10000);
        const int failures = recovery.reconnect_attempts();
        recovery.OnReconnecting(2001 + i * 10000);
        recovery.OnDisconnected(2002 + i * 10000);
        assert(recovery.reconnect_attempts() == failures);
        recovery.OnConnected(3000 + i * 10000);
        recovery.OnConversationStarted(3500 + i * 10000);
        assert(recovery.reconnect_attempts() == failures);
    }
    assert(eidolon::ChannelReconnectDelayMs(recovery.reconnect_attempts()) == 30000);
    recovery.OnDisconnected(133000);  // 60 seconds since the last connect
    assert(recovery.reconnect_attempts() == 0);
    recovery.OnNetworkLost();
    assert(!recovery.TrySchedule(false, true));
    recovery.OnNetworkRestored();
    assert(recovery.TrySchedule(false, true));
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
        assert(eidolon::ChannelReconnectDelayMs(static_cast<int>(i)) == expected_delays[i]);
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
    assert(h.recovery.reconnect_attempts() > 0);
}

void TestConversationRecoveryNeverBecomesStandbyUnreachable()
{
    assert(!eidolon::ShouldEnterStandbyServerUnreachable(1, false));
    assert(eidolon::ShouldEnterStandbyServerUnreachable(2, false));
    assert(!eidolon::ShouldEnterStandbyServerUnreachable(2, true));
    assert(!eidolon::ShouldEnterStandbyServerUnreachable(20, true));
}

void TestRegistrationCredentialsWinAndGenerationNeverRollsBack()
{
    eidolon::Esp32HubConfig registration;
    registration.status = eidolon::HubConfigStatus::Active;
    registration.session = Room("registration-token", "device-identity");
    const eidolon::RoomConfig stale_runtime = Room("stale-runtime-token", "guard-identity");

    auto selected = eidolon::BuildChannelConnectionConfig(registration, &stale_runtime);
    assert(selected.session.token == "registration-token");
    assert(selected.session.identity == "device-identity");

    registration.status = eidolon::HubConfigStatus::WaitingBinding;
    selected = eidolon::BuildChannelConnectionConfig(registration, &stale_runtime);
    assert(selected.session.token == "stale-runtime-token");

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
    // A participant update is only a hint: a new clock observation and
    // identical config must not retire a healthy media graph.
    {
        eidolon::Esp32HubConfig current;
        current.status = eidolon::HubConfigStatus::Active;
        current.session = Room("token");
        current.session.server_urls = {"wss://livekit.test"};
        current.output_policy = {true, 7, 3, true, true};
        current.expires_at_ms = 1700000100000LL;
        auto fresh = current;
        fresh.clock.Observe(1700000000000LL, 50, 55);
        assert(fresh.clock.Now(55) != current.clock.Now(55));
        assert(!eidolon::ChannelConfigurationChanged(current, fresh));
        const auto changed = [&](auto mutate) {
            auto candidate = current;
            mutate(candidate);
            assert(eidolon::ChannelConfigurationChanged(current, candidate));
        };
        changed([](auto& c) { ++c.output_policy.revision; });
        changed([](auto& c) { c.output_policy.microphone = false; });
        changed([](auto& c) { c.output_policy.allowed = 1; });
        changed([](auto& c) { c.status = eidolon::HubConfigStatus::WaitingBinding; });
        changed([](auto& c) { c.session.token = "rotated"; });
        changed([](auto& c) { c.session.identity = "replacement"; });
        changed([](auto& c) { c.session.room_name = "replacement"; });
        changed([](auto& c) { c.session.server_url = "wss://other.test"; });
        changed([](auto& c) { c.session.server_urls.push_back("wss://other.test"); });
        changed([](auto& c) { ++c.expires_at_ms; });
        changed([](auto& c) { c.sample_rate = 48000; });
        changed([](auto& c) { c.channels = 2; });
    }
    TestStandingChannelOpenAdmission();
    TestExpiredCredentialsStillRecoverAfterRefreshFailures();
    TestRecoveryRespectsAuthorityAndNetworkBoundaries();
    TestFlappingConnectionPreservesBackoffUntilStable();
    {
        eidolon::ChannelRecovery recovery;
        assert(recovery.NextAddressIndex(2) == 0);
        recovery.OnDisconnected();
        assert(recovery.NextAddressIndex(2) == 1);
        recovery.OnConnected();
        recovery.OnDisconnected();
        assert(recovery.NextAddressIndex(2) == 1); // Prefer last successful route.
        recovery.OnNetworkRestored();
        assert(recovery.NextAddressIndex(2) == 0);
        assert(recovery.NextAddressIndex(1) == 0); // Refreshed binding shrank.
    }

    TestConnectedTerminalSchedulesAndRecovers();
    TestInitialSynchronousFailureRetries();
    TestFailedAndDisconnectedDeduplicate();
    TestSdkReconnectIsBoundedAndCanSelfHeal();
    TestVoiceHandoffDropsOldGenerationAndQueuedTimer();
    TestNetworkLostRestoredOwnsRecovery();
    TestBackoffRediscoveryAndServerUnreachable();
    TestConversationRecoveryNeverBecomesStandbyUnreachable();
    TestRegistrationCredentialsWinAndGenerationNeverRollsBack();
    return 0;
}
