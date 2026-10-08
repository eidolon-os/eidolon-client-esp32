# Xiaoling voice bring-up: RTC follow-up

## Observed failure

After BOOT, the device attempted to join its assigned room but repeatedly reported
`Failure: RTC`. An InRoom UI state alone was not evidence of a working media
connection. The peer had IPv6 disabled while IPv6 remote ICE candidates were
still passed into esp_peer. Peer failure followed candidate processing.

## Candidate fix and completed checks

The vendor SDK's `eidolon_dev` commit
`b7717a1cc3f913d8d460c6ed1c04c830181a27b9` filters remote IPv6 candidates when
the peer has IPv6 disabled, using the actual peer configuration. This belongs in
the SDK transport boundary, not the xiaoling board or Host configuration.

- ICE candidate transport tests passed with ASAN/UBSAN.
- Receive-only peer regression test passed.
- ESP-IDF 5.5.4 xiaoling build passed.
- Application-only flash and partition readback verification passed.
- The device boot stamp reported the exact SDK commit above and board xiaoling.
- Wi-Fi credentials, device identity and claim were preserved.

The SDK commit is local and has not been pushed. This build used a temporary,
per-command Git URL rewrite to `/Users/manson/ai/eidolon/vendor/client-sdk-esp32`.
The manifest pin is not yet available to fresh GitHub-only builds; publish the
SDK commit before distributing this dependency pin.

## Hardware verification update (2026-10-08)

The boot stamp again confirmed SDK `b7717a1`. After provisioning on the current
network, the user confirmed room entry and an audible startup cue. Serial capture
`/tmp/xiaoling-voice-20261008.log` includes `SRTP connected OK` and a connected
LiveKit room. Host trace turn `6c76095055d24d21` at 10:26:10 +08:00 contains a
final ASR transcript, proving microphone publication reached the recognizer.

That turn failed before reply generation: the assigned companion's stored
conversation preferences failed Agent validation. Host fixes were committed as
Data `9e25d05` and Channel `8471949` and deployed to the Mac.

Subsequent turns `8f9c0b6e05a24788` and `64f98af7a516402a` completed ASR, reply
generation, TTS and playback without Agent errors. The recognized requests were
"你好。" and "你给我讲个故事吧。"; Host speech-stop-to-first-audio latency was
3.092 s and 2.808 s, with playback lasting 6.597 s and 50.751 s. Device logs show
the mic muted during playback and reopened afterward, confirming half-duplex
gating. The user reported two conversational turns.

Remaining findings: both expression commands were rejected as COMMAND_EXPIRED;
the listening UI precedes mic reopening by about 1.25 s. The shared Host idle
timer incorrectly counted playback time; Channel `42faa60` was tested and
deployed to restart the idle window at the end of assistant work, but its full
60-second hardware retest is pending. External-chip wake and audible error
fallback also still require hardware verification.

## Earlier verification interruption

After flashing, both the ESP32 serial device and the Android ADB device disappeared
from the Mac. Serial capture ended with `Device not configured` before a new BOOT
and speech test could be observed at that time.

Next verify: stable RTC connection, local startup cue, microphone publication and
ASR, reply playback, half-duplex turn taking, and external-chip wake. The mic and
speaker pin/slot configuration matches the supplied reference board. No audio
hardware changes were made based solely on the RTC failure.

Local diagnostic files (not checked in, may contain session credentials):
`/tmp/xiaoling-voice-current.log`, `/tmp/xiaoling-rtc-build.log`,
`/tmp/xiaoling-rtc-flash.log`, `/tmp/xiaoling-rtc-fixed.log`.

## 2026-10-08 shared room-open latency optimization

The previous warm-standby BOOT request spent 11.064 s in Hub Resume before
sending session_open, reached Listening at about 13.7 s and the ready cue at
about 17.3 s. The Host additionally serialized TTS pool warmup (1.545 s in that
session) ahead of AgentSession media startup.

The shared controller now reuses a connected Active binding only when its
boot-local clock is known, the credential is unexpired, no invalidation is
pending, and the identity is not rejected. Other cases use existing recovery;
a failed recovery remains invalidated rather than falling back to that snapshot.
Opening is displayed before recovery starts. The Provider's existing locked
per-open authorization and Agent runtime binding resolution remain authoritative.
No board-specific latency policy, token cache or renewal timer was added.

Channel's StreamingPipeline (used by xiaoling half-duplex and box-3) now overlaps
existing provider prewarm with AgentSession startup using a scoped TaskGroup.
Both operations finish before session confirmation. The public Agent on_enter
callback waits until that confirmation has been sent before scheduling welcome
audio. No private LiveKit media interface or replacement connection pool is used.
A media_started trace mark separates media startup from warmup_done.

Validation: six device test runners passed, including the actual controller
DoJoinRoom body, lease renewal, permission invalidation and configuration snapshot
handlers under ASan/UBSan where supported. Channel's applicable authorization,
startup, failure/cancellation, route and real streaming welcome tests: 203 passed.
An expanded welcome matrix still has the previously identified PTT fixture issue
(missing _destination; 26 failures in that selected run), unrelated to this change.

Deployment: xiaoling app-only flash verified partition readback, boot selection
and firmware build stamp (2026-10-08 11:41:00, SDK b7717a1). Mac Channel restarted
through eidolond; all 15 Host health checks passed. The first post-flash boot
experienced repeated LAN Hub connection timeouts; a subsequent ordinary reset
restored Hub activation and standing RTC, operational_ready=1 at ~14.5 s uptime.
The cause of that transient failure is not established. Wi-Fi/identity were retained.

Follow-up hardware tests: xiaoling opened twice at 12:14 and produced three
recognized turns and replies. Host job-to-confirmation was 0.532/0.423 s;
job-to-device playback-state telemetry was 1.26/1.07 s. Device serial capture
had expired before that test, so these are not BOOT-to-audible-cue measurements.
The user confirmed both session terminations were intentional BOOT exits.

Box-3 was subsequently app-only flashed, build stamp 2026-10-08 12:24:13,
with the same SDK, preserving identity, Wi-Fi, full duplex and device AEC.
Its two device-request-to-Listening measurements were 1.04/about 1.00 s.
Both opens logged reuse_channel=1 with preparation elapsed_ms=0. First entry
reached a nonzero cue playback sample at 2.24 s; the second cue had playback
state but no captured nonzero sample, so audible delivery remains unconfirmed.
Three turns (哈喽 / 没啥 / 你在哪儿) took 3.704/1.380/1.525 s from speech stop
to Host first reply audio. The greeting included 2.073 s from final transcript
to turn commit; this endpointing delay remains to investigate. The last reply
was cut short by intentional device leave. The full 60-second idle hardware
regression is still pending. Box-3 logs: /tmp/box3-open-optimization-runtime.log.
Local logs: /tmp/xiaoling-open-optimization-{build-final,flash,runtime,reboot}.log;
/tmp/room-open-host-tests-final.log; /tmp/room-open-host-status.json.

## 2026-10-08 xiaoling acceptance follow-up

External-chip wake is physically confirmed: the user said “你好，小灵”, heard
the room cue and received a long story. A later pair of UART wake detections
opened only one room; the user reported possibly saying two wake phrases.
There is no evidence here of an unsolicited duplicate wake-chip event.

The first unattended story revealed a shared Channel issue: transcriptless VAD
candidates caused a “没听清” fallback, whose playback renewed the idle window.
Channel now discards those candidates without speaking. Actual terminal output
failures retain their existing once-per-turn spoken fallback. Regression tests
cover repeated transcriptless noise and a real AgentSession producing non-silent
audio through a scripted TTS provider after a terminal error: 55 tests passed.
The latter is integration coverage, not a hardware acoustic fault-injection test.

After deployment, trace esp32-8d62f61e-ed9715c6-00000001 played a 76.656-second
reply without the 60-second timer cutting it off. Host playback ended at
13:12:36.060; idle disconnect fired at 13:13:36.059 (59.999 seconds later).
The device received idle_normal_end and returned to ConfigReady. A subsequent
empty VAD candidate neither produced fallback audio nor extended that deadline.

The HTTP Date observation previously included DNS/TCP/TLS setup in its clock
uncertainty, which could falsely expire short-lived expression commands. The
observation now starts at IDF HTTP_EVENT_ON_CONNECTED, before HTTP request data
is sent. Conservative Date rounding, HTTP elapsed time and expiry enforcement
remain intact. Production-handler ASan/UBSan tests cover slow TLS, slow HTTP,
missing connection events and invalid dates; deadline and date-parser tests pass.
Xiaoling was app-only flashed with build stamp 2026-10-08 13:08:26, SDK b7717a1.
Box-3 has not received this clock-boundary change yet. The two story turns chose
presentation.intent=none, so they do not establish successful expression receipt
or visible animation; an actual expression-producing turn remains pending.

An official Ops restart of LiveKit tested standing-channel transport recovery:
operational_ready changed to 0 at device uptime 305.961 seconds and back to 1
at 311.431 seconds, a 5.470-second recovery without reprovisioning. This validates
RTC service outage recovery, not Wi-Fi access-point loss. All 15 Mac Host health
checks passed after the test. Wi-Fi AP-loss recovery and audible terminal-error
fallback on the physical device remain unverified.

Local evidence: /tmp/xiaoling-acceptance-fixed.log,
/tmp/xiaoling-acceptance-{build,flash,error-audio-tests}.log,
/tmp/xiaoling-acceptance-livekit-restart.json,
/tmp/xiaoling-acceptance-final-host-status.json.

## 2026-10-08 automated physical acceptance (14:54–15:00)

Mac's Tingting speech output successfully woke the independent chip and supplied
“我今天有点难过，你能安慰我一下吗？” through the real microphone. In session
esp32-86a0245d-1ee9fc10-00000001, the expression receipt reached completed,
sequence 3, elapsed_ms=1875, with no expiry rejection. CompanionFaceView emits
these receipts after its display flush boundary, so this validates device-side
render execution. No camera inspection of the actual pixels was performed.

For terminal-error audio, the verified local Agent RPC process was temporarily
paused with finally-based recovery and an independent timed resume guard. The
real microphone request reached the existing 30-second first-delta timeout.
Session esp32-823e8c20-bbd23f22-00000002 recorded recoverable=false and
silent_failure_fallback={attempted:true, spoken:true,
reason:llm_error_without_delta}. The device entered playback at 213.311 seconds,
reported nonzero playback RMS (0.103 and 0.127), and returned to silent at
216.721 seconds. The existing fallback text is “刚才卡了一下，请再说一遍好吗？”.
This closes the real transport/device audio-output test; no independent acoustic
recording was made. The Agent process was resumed successfully.

Wi-Fi link-loss injection remains unverified. An attempted USB JTAG/GDB call to
the existing esp_wifi_disconnect API did not reach the function: the Python GDB
variant crashed locally, and the no-Python attempt failed in OpenOCD flash-map
probing, triggering device cache/panic errors. These were induced during debug
attachment, not an observed normal-operation Wi-Fi failure. Debugging was stopped;
the official serial reset restored the unchanged installed firmware and existing
identity/Wi-Fi. Operational readiness returned at 14.503 seconds after reset.
No Flash write, erasure, router change or product-code change was made. All 15
Mac Host health checks were healthy afterward. AP-loss recovery needs a working
AP-control path or a separate controlled hardware test; RTC outage recovery above
must not be substituted for it.

Evidence: /tmp/xiaoling-auto-acceptance.log, /tmp/xiaoling-auto-restored.log,
/tmp/xiaoling-auto-final-host.json, /tmp/xiaoling-wifi-disconnect-gdb.log,
/tmp/xiaoling-jtag-wifi.log. Raw serial logs may contain transient RTC credentials;
they remain local and are not checked in.

## 2026-10-08 real router power-cycle acceptance — failed

The user power-cycled the Wi-Fi router while the unchanged xiaoling firmware
was in standby. USB capture /tmp/xiaoling-ap-power-cycle.log was active before
the outage; no debugger or fault injector was attached during this test.

Capture-relative timing (seconds):

- 146.831: Wi-Fi disconnect reason 200; readiness cleared at 146.841.
- 171.271: saved SSID rediscovered, now on channel 1 instead of 11.
- 172.531: DHCP acquired 192.168.3.41 instead of 192.168.3.38, 25.700 seconds
  after the disconnect. Credentials were reused without user action.
- 189.351: Hub mDNS resolved the Mac's new 192.168.3.40 address instead of
  192.168.3.228, after an initial lookup timed out while networking recovered.
- 193.691: RTC connection attempted against the correct new Host address.
- 220.191: adapter timed out waiting for room disconnect.
- 225.341: SDK logged “Engine task did not exit in time; forcing deletion”.
- 226.361: a subsequent connection attempt began; at 226.731 the device
  panicked with an interrupt watchdog timeout and rebooted itself.

The matching ELF decodes CPU1's stack to peer_task (peer.c:74), through
media_lib_event_group_set_bits / xEventGroupSetBits / spinlock_acquire.
SDK engine_destroy can force-delete its engine task and then call peer_destroy
directly; peer_destroy deletes the event group and frees the peer without joining
its task. This is a concrete lifetime hazard consistent with the observed crash;
the exact interleaving still needs a deterministic regression test and fix in
vendor/client-sdk-esp32, not a board-specific workaround.

After reboot, Wi-Fi and mDNS again succeeded but Hub HTTPS opens repeatedly
timed out (TLS error 0x8006). Local Mac checks report healthy services and its new
LAN Hub endpoint returns HTTP 200, while three Mac-to-device pings received no
reply. These observations do not establish the cause of the post-reboot network
failure. Later Host ICE logs include its new IP; the deeper comparison below
corrects the initial inference that this ruled out stale ICE addresses throughout
the outage. Packet-header capture was unavailable without sudo
authentication. Full automatic recovery and post-outage conversation have not
passed; retain this failure even if a later manual intervention restores service.

### Cross-device root-cause comparison

The Host timeline shows both devices initially attempted reconnection before
LiveKit completed its network refresh. Xiaoling joined at 15:08:29.851 and box-3
at 15:08:30.845. The old server sent ICE candidates advertising 192.168.3.228
at 15:08:35.108 and 15:08:36.193 respectively, although Hub discovery already
advertised the Mac's new 192.168.3.40 address. This is a real signalling/media
readiness mismatch, not a failed device mDNS lookup.

The eidolond read-only audit confirms system.runtime.restart for livekit at
2026-10-08T07:08:32.312094Z. LiveKit logged shutdown at local 15:08:32.368 and
startup with nodeIP=192.168.3.40 at 15:08:42.441. Box-3 joined the new instance
at 15:08:42.898, selected a UDP host pair (.40 to .42) at 15:08:43.241, and became
active at 15:08:43.638. Its recovery was therefore not a successful first attempt.
Xiaoling instead remained stuck on the older connection's teardown, hit the
adapter deadline and engine forced-deletion path, and panicked on the next retry.
The exact blocking call inside the old engine was not captured, and must not be
attributed to half/full duplex, CPU load or heap pressure without evidence.

Both installed build stamps identify SDK b7717a1 and IDF 5.5.4. Both boards use
WifiBoard and the shared EidolonVoiceController network recovery. Their complete
firmware images are not identical: hardware/audio profiles differ, and xiaoling
also has the later HTTP clock-boundary change. No board-specific network-recovery
branch was found. The compared effective Wi-Fi/LwIP/FreeRTOS/LiveKit settings do
not differ; TLS certificate-bundle selection differs but no certificate rejection
was observed. SDK peer.c and engine.c match the vendor checkout byte-for-byte.

A bounded host reproduction extracts unchanged production peer_task and
peer_destroy function bodies, substitutes the RTOS/transport with a pthread
barrier, and invokes peer_destroy while the peer task remains inside its loop,
as permitted by engine_destroy's forced-exit path. AddressSanitizer deterministically
reports heap-use-after-free when the peer resumes. This validates the unsafe
lifetime sequence independently of either board; it does not reproduce the
unknown original engine blocking call or the exact ESP32 watchdog symptom.
Local reproduction: /tmp/xiaoling-peer-lifetime-repro.c and .log.

The device eventually logged operational_ready=1 at capture time 704.411 seconds
(557.570 seconds after readiness was lost), without another captured boot.
Whether the user made any intervening network changes has been asked but is not
yet confirmed. This late readiness does not convert the watchdog test into a pass.

### Minimal repair boundary (analysis, not deployed)

The old ICE-address window is a transient input that retry must tolerate. Keep
the existing eidolond network refresh and device ChannelRecovery backoff; no new
Host/device coordination protocol or board-specific recovery branch is justified
by this test alone.

The concrete SDK contract conflict is more important: signaling send_request
passes portMAX_DELAY to WebSocket send (including Leave), and WebSocket close's
250-ms argument does not bound its whole call. The installed WebSocket component
sends the close frame with portMAX_DELAY and can subsequently wait indefinitely
for STOPPED_BIT. Meanwhile engine_destroy waits only 5000 ms, force-deletes the
engine task, and releases peer resources without ensuring the peer has exited.
The public room destroy ignores the engine destruction result and frees its own
state. The adapter likewise clears its handle even if destruction reports failure.
Increasing those wait constants does not repair this ownership violation.

Preferred scope: cooperative, bounded/cancellable signalling shutdown using
existing transport stop primitives, and one safe peer stop/join/free sequence
shared by close, backoff, connect failure and destruction. Never free resources
owned by an unjoined task. If shutdown is not complete, propagate that result and
retain ownership; the existing recovery mechanism must finish closing the old
instance before admitting a new connection. Do not add another retry scheduler,
parallel room, intentional board reboot, or a leaked-object fallback.

Acceptance should exercise both devices against a delayed/blackholed signalling
transport and a server restart during ICE establishment, with delayed peer exit.
Assertions: no freeing before task completion; no overlapping attempts; retained
callbacks stay valid until shutdown; recovery proceeds once the transport is back;
no reboot or memory growth over repeated cycles. A narrow diagnostic at shutdown
phase boundaries is still needed to identify the exact engine blocking call in
the physical failure; the current evidence must not claim that specific call has
already been observed.

## 2026-10-08 SDK upstream sync and shutdown ownership repair

Fetched official upstream/main at d492fa8. The two commits beyond the existing
base update example-agent Python dependencies only; they do not change RTC
teardown. Merged upstream into the clean local eidolon_dev branch as ebf1284,
preserving the existing fork changes. Fork origin/eidolon_dev was also fetched
and confirmed to be an ancestor, with no divergent remote changes.

SDK repair d10c950a4b325c3af5b619f35ad72ff7677c8f87 makes shutdown cooperative:
the engine task cleans up transport workers before reporting completion. A
5-second join timeout now returns an error with ownership retained, rather than
force-deleting the engine. The public room API propagates that result and keeps
the room/callback context valid. Peer destruction always stops and joins its loop
before closing the native connection and freeing the event group, including a
failed connect. A failed thread creation clears the running flag. Cross-task
stop flags are atomic. Signal sends use the existing finite network timeout, and
transport teardown uses the existing WebSocket stop/join API without waiting for
a remote close handshake.

The shared ESP32 LiveKitSession adapter retains its room and media on a pending
shutdown. Connect must successfully close that room before updating generation,
identity or media resources and creating its replacement. Existing controller
backoff remains the retry owner; no new timers, room instances, board-specific
branch or reboot policy were introduced. The manifest pins the new SDK commit;
managed components are resolved by the component manager, not edited directly.

Validation so far: all seven SDK host tests passed, including ASan/UBSan delayed
peer-loop exit across 32 cycles, failed connection/thread creation, engine join
timeout followed by retry, retained public-room context and bounded signalling.
ICE candidate regression passed. The actual adapter Disconnect body/Connect
admission guard passed ASan/UBSan ownership tests; channel recovery and Hub clock
boundary regressions passed. Physical AP power-cycle acceptance still requires
the newly built firmware and must not be inferred from host tests.

Both xiaoling and box-3 full ESP-IDF 5.5.4 builds subsequently passed against
the new pin. The component-manager lock resolves d10c950 and the managed source
matches that SDK commit. Fork eidolon_dev was pushed without force; a fresh
ls-remote read confirmed d10c950a4b325c3af5b619f35ad72ff7677c8f87, and the local
branch is synchronized with origin. No device was flashed during this SDK
update, so router-power-cycle validation of this fix is still pending.
Build logs: /tmp/xiaoling-shutdown-fix-build.log and
/tmp/box3-shutdown-fix-build.log.
