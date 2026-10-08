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
