#include "hub_activation_retry_core.h"

namespace eidolon {

namespace {

// 10, 20, 40, 80, then 120 for as long as the device keeps asking. Expressed as
// a function of the attempt count so the schedule cannot drift with the loop.
int DelayForRetryableAttempts(int retryable_attempts)
{
    if (retryable_attempts <= 0) {
        return HubActivationRetryCore::kFirstDelaySeconds;
    }
    int delay = HubActivationRetryCore::kFirstDelaySeconds;
    for (int i = 1; i < retryable_attempts &&
                    delay < HubActivationRetryCore::kMaxDelaySeconds;
         ++i) {
        delay *= 2;
    }
    return delay > HubActivationRetryCore::kMaxDelaySeconds
               ? HubActivationRetryCore::kMaxDelaySeconds
               : delay;
}

}  // namespace

ActivationStandDown HubActivationRetryCore::Latch(ActivationStandDown reason)
{
    if (stand_down_ == ActivationStandDown::KeepAsking) {
        stand_down_ = reason;
    }
    return stand_down_;
}

ActivationStandDown HubActivationRetryCore::Evaluate(bool commissioning_in_progress)
{
    if (stand_down_ != ActivationStandDown::KeepAsking) {
        return stand_down_;
    }
    if (commissioning_in_progress) {
        return Latch(ActivationStandDown::CommissioningOwnsRadio);
    }
    return ActivationStandDown::KeepAsking;
}

ActivationStandDown HubActivationRetryCore::OnAttempt(ActivationAttemptOutcome outcome)
{
    if (stand_down_ != ActivationStandDown::KeepAsking) {
        return stand_down_;
    }
    switch (outcome) {
    case ActivationAttemptOutcome::Admitted:
        return Latch(ActivationStandDown::Admitted);
    case ActivationAttemptOutcome::ClaimTerminal:
        return Latch(ActivationStandDown::ClaimTerminal);
    case ActivationAttemptOutcome::Retryable:
        break;
    }
    ++retryable_attempts_;
    delay_seconds_ = DelayForRetryableAttempts(retryable_attempts_);
    return ActivationStandDown::KeepAsking;
}

}  // namespace eidolon
