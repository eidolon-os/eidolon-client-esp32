# BOX-3 acoustic stutter comparison

These tools prepare and observe real device playback. They do not fabricate
playback acknowledgements or publish an alternate diagnostic audio track.

## Status of the initial preparation (2026-10-14)

Two images were built from client commit `f2ff53f6` with SDK `d10c950a4b32`.
Their sdkconfigs differed only in the 150 ms versus 0 ms playout threshold.
Both enabled automatic full-duplex session entry, existing RX diagnostics and
an extra periodic heap log. Both images passed image checksum validation.
**Neither image was flashed; no automated hardware conversation was run.**
Concurrent playback changes in another task required exclusive-device
coordination before proceeding. This is preparation evidence, not a claim
that either setting fixes stutter or rules out memory pressure.

## Preparation

Use a disposable detached worktree at the desired baseline revision, with the
normal local vendor SDK checkout available at the location required by that
revision. Reserve the BOX-3 before flashing or opening its serial port.

```sh
bash tests/hil/stutter/build_variant.sh /absolute/path/to/worktree 150
# Save build/eidolon/esp-box-3/eidolon.bin and sdkconfig.esp-box-3 externally.
bash tests/hil/stutter/build_variant.sh /absolute/path/to/worktree 0
# Save this image/config too; verify the config diff before using either.
```

The builder applies `heap-diagnostics.patch` only in the supplied detached
worktree. It does not change LiveKit SDK code and does not flash anything.
The patch logs the largest internal free block and PSRAM free/largest block
alongside the existing internal free/minimum heap values. Keep it enabled in
both comparison variants. Do not merge this temporary logging into production.

On macOS, generate and check a fixed acoustic request:

```sh
say -v Tingting -r 180 -o /tmp/story-request.aiff \
  '请讲一个小狐狸寻找星星的故事，详细讲述，持续大约一分钟。'
afinfo /tmp/story-request.aiff
```

Back up the installed firmware and preserve identity/NVS. Flash the selected
image through the project's partition-aware workflow. The ordinary board
wrapper forces auto-entry off, so do not rebuild the variant with that wrapper
before flashing. Restore the original firmware when testing finishes.

## Run and interpretation

Requires macOS `afplay`, Python and `pyserial`. Keep the speaker audible to the
BOX microphone. Opening the serial port can reset the board; capture from boot.
Capture Host ASR and speech lifecycle logs separately for the same time window.

```sh
python tests/hil/stutter/run_acoustic.py --request /tmp/story-request.aiff \
  --out /tmp/stutter-150 --port /dev/cu.usbmodem1101 --rounds 3 --seconds 480
python tests/hil/stutter/summarize_capture.py /tmp/stutter-150/box3.jsonl \
  --out /tmp/stutter-150/summary.json
```

The runner requires a real `session_started`, waits for idle playback, and
spaces requests by at least 100 seconds. It stops if no session or audible
reply is observed. Create a file named `stop` in the output directory to stop
a run. Use a fresh output directory for each run.

Accept a round only after Host logs confirm the intended transcript and a real
reply, and device logs confirm nonzero playback. Exclude interruption, reset,
reconnect and concurrent Host changes from a controlled comparison. Use the same
Host, SDK, fixed request, device position, volume and diagnostics in both arms.

RX gaps include source silence and DTX; they are not packet-loss counts. Playback
RMS does not prove every syllable was audible. A low heap watermark does not
prove allocation failure. Correlate heap trends/largest blocks, source speech
timing and device output timing before assigning a cause.
