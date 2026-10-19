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

## Confirmed media grants across reconnect

`DeviceOutputGate` owns the accepted session ID and output/input selection.
Its suspension flag describes whether that grant may currently execute; it is
not another conversation state machine.

- `Suspend()` blocks all input/output but retains the accepted grant. SDK repair,
  disconnect/failure, Wi-Fi loss and connection watchdog recovery use it.
- `Resume(session_id)` requires the same accepted session. `Bind()` already
  invalidates grants on a changed or rejected policy, so recovery cannot widen
  permissions or reuse an obsolete revision. Legacy devices without policies
  still rely on the controller's confirmed conversation.
- `Close()` forgets the grant. Leave, session-end, commissioning and abandoned
  open requests remain terminal. Suspension also cancels current presentation
  and motion execution; it does not replay old output on recovery.
- A valid confirmed conversation is re-announced with its existing ID and its
  media restored without waiting for an acknowledgement replay. The Provider's
  idempotent active-session response does not promise another `session_started`.
- If that grant is no longer valid, recovery opens a **new conversation ID** and
  waits for a newly authorized plan. This is a permission-change session boundary,
  not normal reconnect behavior; continuity of the old Agent session is not
  promised across that boundary. The existing Provider contract forbids changing
  the plan of an active same-ID session. Device-owned old sessions can be
  superseded; orchestrator/team-owned sessions retain their conflict protection.
- A queued `session_started` cannot open media while the transport is disconnected.
  Existing connection-generation and conversation-ID fences reject stale events.

The LiveKit vendor engine checks remote SDP processing and local SDP sends in
both connecting and connected states. An explicit failure enters its existing
backoff path immediately. No extra retry loop, SDP rewriting, timeout extension,
or per-conversation transport rebuild is introduced. This addresses discarded
errors; it does **not** establish why the October 18 hardware negotiation failed.

### Local acceptance (2026-10-18)

- `bash tests/run_output_policy_tests.sh`: real gate, policy-required/legacy
  profiles, suspend/resume, wrong session, terminal close and permission changes;
  AddressSanitizer and UndefinedBehaviorSanitizer enabled.
- `python3 tests/run_controller_media_recovery_tests.py`: production lifecycle
  handlers linked to the real gate, with transport/timers/board IO isolated;
  100 alternating SDK/full reconnect cycles, Wi-Fi loss, watchdog, duplicate,
  stale/offline/untrusted controls, input-off recovery, permission renewal,
  Leave during reconnect, open timeout and failed re-announcement.
- Existing configuration snapshot, permission refresh, channel recovery, pending
  controls, audio renderer, playback state, UI projector and commissioning tests.
- Vendor `python3 -m unittest discover -s tests -p 'test_*.py' -v`: 8 tests,
  including negotiation error injection, worker ownership and receive-only mode.
- Provider adapter/output-policy compatibility suite: 157 passed.
- `bash scripts/eidolon/eidolon-esp-box-3.sh build`: ESP-IDF 5.5.4 BOX-3 build
  and partition checks passed. No firmware was flashed during this acceptance.

These are host unit/integration tests and a target firmware build, not a hardware
end-to-end result. The original inactive-to-new-track SDP timeout still requires
on-device reproduction with a pinned build identity before it can be closed.
