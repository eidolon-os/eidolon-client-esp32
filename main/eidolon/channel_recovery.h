#ifndef EIDOLON_CHANNEL_RECOVERY_H_
#define EIDOLON_CHANNEL_RECOVERY_H_

#include <cstdint>

#include "hub_types.h"

namespace eidolon {

// Platform-independent recovery state for the device's one channel. The
// controller owns one of these on its actor task; keeping the retry admission
// rules here makes the Failed/Disconnected/network/handoff races testable
// without an ESP32 or LiveKit.
//
// It used to be called ControlRoomRecovery, from when a device stood in one of
// two rooms and this tracked the control one. There is one channel now, and
// what this still distinguishes is standby from a live conversation — the
// device does not move, so it says which of the two it is in.
class ChannelRecovery {
public:
    enum class Phase {
        Idle,
        Connecting,
        Connected,
        Reconnecting,
    };

    void OnActivation()
    {
        network_available_ = true;
        phase_ = Phase::Idle;
        reconnect_pending_ = false;
        reconnect_attempts_ = 0;
    }

    void OnNetworkLost()
    {
        network_available_ = false;
        phase_ = Phase::Idle;
        reconnect_pending_ = false;
        reconnect_attempts_ = 0;
    }

    void OnNetworkRestored() { OnActivation(); }

    void OnAttemptStarted() { phase_ = Phase::Connecting; }
    void OnConnecting() { phase_ = Phase::Connecting; }
    void OnReconnecting() { phase_ = Phase::Reconnecting; }

    void OnConnected()
    {
        phase_ = Phase::Connected;
        reconnect_pending_ = false;
        reconnect_attempts_ = 0;
    }

    void OnConversationStarted()
    {
        phase_ = Phase::Idle;
        reconnect_pending_ = false;
        reconnect_attempts_ = 0;
    }

    void OnDisconnected() { phase_ = Phase::Idle; }

    // A deliberate teardown owns the next action. Cancel an ordinary recovery
    // timer so a queued tick cannot race it.
    void OnIntentionalTeardown()
    {
        phase_ = Phase::Idle;
        reconnect_pending_ = false;
    }

    bool TrySchedule(bool switching_to_voice, bool has_channel_config)
    {
        if (!network_available_ || switching_to_voice || reconnect_pending_ ||
            !has_channel_config) {
            return false;
        }
        reconnect_pending_ = true;
        return true;
    }

    void OnScheduleFailed() { reconnect_pending_ = false; }

    // Returns the zero-based attempt used for backoff/escalation and keeps the
    // pending guard held across the synchronous connection call.
    bool BeginRetry(int* attempt)
    {
        if (!network_available_ || !reconnect_pending_ || attempt == nullptr) {
            reconnect_pending_ = false;
            return false;
        }
        *attempt = reconnect_attempts_++;
        return true;
    }

    void FinishRetry() { reconnect_pending_ = false; }

    bool network_available() const { return network_available_; }
    bool reconnect_pending() const { return reconnect_pending_; }
    bool connect_in_flight() const
    {
        return phase_ == Phase::Connecting || phase_ == Phase::Reconnecting;
    }
    bool connected() const { return phase_ == Phase::Connected; }
    int reconnect_attempts() const { return reconnect_attempts_; }
    Phase phase() const { return phase_; }

private:
    bool network_available_ = true;
    bool reconnect_pending_ = false;
    int reconnect_attempts_ = 0;
    Phase phase_ = Phase::Idle;
};

inline uint32_t ChannelReconnectDelayMs(int attempt)
{
    constexpr uint32_t kBaseDelayMs = 1000;
    constexpr uint32_t kMaxDelayMs = 30000;
    uint32_t delay_ms = kBaseDelayMs;
    for (int i = 0; i < attempt && delay_ms < kMaxDelayMs; ++i) {
        delay_ms <<= 1;
    }
    return delay_ms > kMaxDelayMs ? kMaxDelayMs : delay_ms;
}

inline bool ShouldRediscoverChannelConfig(int attempt)
{
    return attempt >= 1;
}

inline bool ShouldSurfaceChannelServerUnreachable(int attempt)
{
    return attempt >= 2;
}

inline bool ShouldEnterStandbyServerUnreachable(int attempt,
                                                bool conversation_desired)
{
    return !conversation_desired &&
           ShouldSurfaceChannelServerUnreachable(attempt);
}

// Registration credentials are authoritative once registration is active.
// Guard runtime credentials remain a fallback only before that point.
//
// There is nothing left to choose between once a device has one channel; what
// remains is which credential is trustworthy yet, which is still a real
// question before registration completes.
inline Esp32HubConfig BuildChannelConnectionConfig(const Esp32HubConfig& registration,
                                                    const RoomConfig* runtime_fallback = nullptr)
{
    Esp32HubConfig selected = registration;
    if (registration.status != HubConfigStatus::Active && runtime_fallback != nullptr &&
        runtime_fallback->usable()) {
        selected.session = *runtime_fallback;
    }
    return selected;
}

}  // namespace eidolon

#endif  // EIDOLON_CHANNEL_RECOVERY_H_
