# Channel recovery ownership

The device has one persistent channel. Conversation intent survives a transport
failure; explicit Leave or a session-end event cancels it.

## Recovery admission and connection admission

- Recovery requires active enrollment, an authority route, and no identity
  rejection. An expired transport binding must not prohibit refreshing it.
- Connecting additionally requires a usable room binding and valid credentials.
- Missing/expired credentials trigger authority discovery and refresh before the
  first retry. Subsequent failures also refresh, allowing routes to change.
- A transient discovery/refresh failure schedules the next bounded backoff on
  the existing ChannelRecovery owner. Approval/binding pending transfers control
  to onboarding polling; identity rejection stops ordinary retries.

The previous recovery gate used connection admission. Consequently an expired
binding could prevent the very retry that would renew it. A second exit path
abandoned retries after a failed refresh. Neither condition proves that the
network caused the original disconnect, but both can prevent recovery afterward.

## Attempt deadlines

SDK reconnect callbacks share one connection watchdog deadline; repeated callbacks
do not extend it. Scheduling application backoff disarms that watchdog. Starting
the next actual connection arms a new deadline. An exhausted attempt disconnects
and supersedes its generation before retrying, preserving conversation intent.
Reconnect timer creation/start failures surface Error instead of pretending that
a retry is scheduled. Existing memory-ceiling and authorization stops remain.

## Validation

Host tests cover expired binding admission, repeated refresh failures, bounded
backoff, network-loss cancellation, authorization boundaries, and existing UI
projection/input behavior. The StackChan ESP-IDF build validates integration.
These checks do not replace device validation or prove SDK calls cannot block.

On a stable network, verify standby, Join, temporary host unavailability and
recovery without another Join, Leave during recovery, and re-entry. Separately
check a credential-expiry/refresh failure sequence and revoked enrollment. Align
`[lifecycle]` logs with channel logs; no secrets need to be logged. Recording and
microphone validation are handled separately.
