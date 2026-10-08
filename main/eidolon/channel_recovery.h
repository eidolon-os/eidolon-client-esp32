#ifndef EIDOLON_CHANNEL_RECOVERY_H_
#define EIDOLON_CHANNEL_RECOVERY_H_

#include <cstdint>
#include <cstddef>

#include "hub_types.h"

namespace eidolon {

// A clock anchor changes on every authenticated pull, but is not a new media
// graph. Credentials, routes, policy and format are graph inputs.
inline bool ChannelConfigurationChanged(const Esp32HubConfig& current,
                                        const Esp32HubConfig& fresh)
{
    return current.status != fresh.status ||
        !(current.output_policy == fresh.output_policy) ||
        current.sample_rate != fresh.sample_rate || current.channels != fresh.channels ||
        current.expires_at_ms != fresh.expires_at_ms ||
        current.session.identity != fresh.session.identity ||
        current.session.room_name != fresh.session.room_name ||
        current.session.server_url != fresh.session.server_url ||
        current.session.server_urls != fresh.session.server_urls ||
        current.session.token != fresh.session.token;
}

inline bool ChannelBindingExpired(const Esp32HubConfig& config, int64_t monotonic_ms)
{
    if (config.expires_at_ms <= 0) return true;
    const int64_t now = config.clock.Now(monotonic_ms);
    // A persisted configuration has no boot-local clock anchor.
    return now > 0 && now >= config.expires_at_ms;
}

// Only a live, authenticated, boot-local binding may bypass recovery on open.
// The Provider still authorizes every session_open against its current state.
inline bool CanOpenOnStandingChannel(const Esp32HubConfig& config, bool connected,
                                     bool refresh_required, bool identity_rejected,
                                     int64_t monotonic_ms)
{
    return connected && !refresh_required && !identity_rejected &&
        config.status == HubConfigStatus::Active && config.session.usable() &&
        config.clock.Now(monotonic_ms) > 0 && !ChannelBindingExpired(config, monotonic_ms);
}

// A standing connection can outlive its join credential. Schedule its renewal
// on the controller actor even when there is no disconnect or user activity.
// Unknown boot-local time has no schedulable deadline; onboarding supplies it.
inline int64_t ChannelBindingRenewalDelayMs(const Esp32HubConfig& config, int64_t monotonic_ms)
{
    if (config.status != HubConfigStatus::Active || config.expires_at_ms <= 0) return -1;
    const int64_t now = config.clock.Now(monotonic_ms);
    if (now <= 0) return -1;
    return now >= config.expires_at_ms ? 0 : config.expires_at_ms - now;
}

// Enrollment permits recovery; disposable transport credentials permit only
// connecting. Expiry must never prevent obtaining their replacement.
inline bool ChannelRecoveryAllowed(HubConfigStatus status, bool has_authority,
                                   bool identity_rejected)
{
    return status == HubConfigStatus::Active && has_authority && !identity_rejected;
}

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
        address_attempt_ = last_address_ = 0;
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
    void OnReconnecting(uint64_t now_ms = 0)
    {
        NoteConnectionLost(now_ms);
        phase_ = Phase::Reconnecting;
    }

    void OnConnected(uint64_t now_ms = 0)
    {
        address_attempt_ = last_address_;
        if (phase_ != Phase::Connected) connected_since_ms_ = now_ms;
        phase_ = Phase::Connected;
        reconnect_pending_ = false;
        // A brief successful handshake does not prove that a flapping link has
        // recovered. Reset the budget only after a sustained connection.
    }

    void OnConversationStarted(uint64_t now_ms = 0)
    {
        OnConnected(now_ms);
    }

    void OnDisconnected(uint64_t now_ms = 0)
    {
        NoteConnectionLost(now_ms);
        phase_ = Phase::Idle;
    }

    // A deliberate teardown owns the next action. Cancel an ordinary recovery
    // timer so a queued tick cannot race it.
    void OnIntentionalTeardown()
    {
        phase_ = Phase::Idle;
        reconnect_pending_ = false;
    }

    bool TrySchedule(bool switching_to_voice, bool recovery_allowed)
    {
        if (!network_available_ || switching_to_voice || reconnect_pending_ ||
            !recovery_allowed) {
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
        *attempt = reconnect_attempts_;
        if (reconnect_attempts_ < 6) ++reconnect_attempts_;
        return true;
    }

    void FinishRetry() { reconnect_pending_ = false; }

    // Transport callbacks keep the existing bounded retry/watchdog ownership.
    // Each failed attempt advances instead of retrying the first route forever.
    size_t NextAddressIndex(size_t count)
    {
        last_address_ = count == 0 ? 0 : address_attempt_++ % count;
        return last_address_;
    }

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
    void NoteConnectionLost(uint64_t now_ms)
    {
        if (phase_ != Phase::Connected) return;
        if (now_ms >= connected_since_ms_ && now_ms - connected_since_ms_ >= 60000) {
            reconnect_attempts_ = 0;
        } else if (reconnect_attempts_ < 6) {
            ++reconnect_attempts_;
        }
    }
    uint64_t connected_since_ms_ = 0;
    size_t address_attempt_ = 0;
    size_t last_address_ = 0;
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

inline bool ShouldRediscoverChannelConfig(int attempt, bool credentials_usable = true)
{
    return !credentials_usable || attempt >= 1;
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
    if (registration.status != HubConfigStatus::Active &&
        registration.status != HubConfigStatus::RecoveryRequired &&
        registration.status != HubConfigStatus::Revoked && runtime_fallback != nullptr &&
        runtime_fallback->usable()) {
        selected.session = *runtime_fallback;
    }
    return selected;
}

}  // namespace eidolon

#endif  // EIDOLON_CHANNEL_RECOVERY_H_
