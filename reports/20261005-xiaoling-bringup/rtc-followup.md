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
