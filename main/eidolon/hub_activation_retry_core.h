#ifndef EIDOLON_HUB_ACTIVATION_RETRY_CORE_H_
#define EIDOLON_HUB_ACTIVATION_RETRY_CORE_H_

namespace eidolon {

// What one attempt to be admitted by the Owner's Host produced.
enum class ActivationAttemptOutcome {
    // The Host admitted the device and handed back a configuration.
    Admitted,
    // Nothing was decided. A Host that is switched off, a network still coming
    // back, an empty discovery, an HTTP error, a Proposal still awaiting the
    // Owner's review — none of these are permanent, so all of them are this.
    Retryable,
    // The Claim reached a terminal state: revoked, removed, or bound to an
    // Authority this device is no longer part of. No amount of asking changes
    // that; only physical presence at the device does.
    ClaimTerminal,
};

// Why the activation loop stopped asking, or KeepAsking while it has not.
enum class ActivationStandDown {
    KeepAsking,
    Admitted,
    CommissioningOwnsRadio,
    ClaimTerminal,
};

// The retry policy for Hub activation: what ends the loop, and how long to wait
// before asking again.
//
// The device's DeviceState is deliberately not an input here, and must not
// become one. DeviceState is a projection of what the screen is showing, and
// the presentation of a device waiting to be approved passes through Idle while
// activation is still in flight. Ending the loop on "the device looks idle"
// therefore ended it one second into the first backoff, on a device that had
// never been admitted — which left an Owner who removed their device with no
// way back except erasing NVS, and erasing NVS mints a new device identity.
//
// What ends the loop is a fact about the activation itself: it succeeded, a
// commissioning generation took the radio, or the Claim is terminal.
class HubActivationRetryCore {
public:
    static constexpr int kFirstDelaySeconds = 10;
    static constexpr int kMaxDelaySeconds = 120;

    // Evaluated before each attempt and on every second of the wait between
    // attempts. Commissioning owns the radio exclusively: an attempt may
    // neither start nor keep waiting while a commissioning generation holds it.
    ActivationStandDown Evaluate(bool commissioning_in_progress);

    // Records the outcome of one attempt.
    ActivationStandDown OnAttempt(ActivationAttemptOutcome outcome);

    // Seconds to wait before the next attempt: 10, doubling, capped at 120.
    int delay_seconds() const { return delay_seconds_; }

    // Attempts that ended in Retryable so far.
    int retryable_attempts() const { return retryable_attempts_; }

    ActivationStandDown stand_down() const { return stand_down_; }

private:
    ActivationStandDown Latch(ActivationStandDown reason);

    ActivationStandDown stand_down_ = ActivationStandDown::KeepAsking;
    int delay_seconds_ = kFirstDelaySeconds;
    int retryable_attempts_ = 0;
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_ACTIVATION_RETRY_CORE_H_
