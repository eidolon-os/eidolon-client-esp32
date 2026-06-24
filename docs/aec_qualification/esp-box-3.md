# ESP32-S3-BOX-3 AEC Qualification

## Conclusion

ESP32-S3-BOX-3 passes standalone device-side AEC qualification for `barge-in`
mode when tested with the current xiaozhi-compatible audio mapping:

- Board: `esp-box-3`
- Qualification reference channel: `refch1`
- Decision: `barge-in`
- Recommended LiveKit mode: `barge-in`

Do not use `refch2` or `refch3` for BOX-3 qualification or business firmware
unless the codec channel ordering is intentionally changed and re-qualified.

## Evidence

The BOX-3 audio path in xiaozhi uses:

- `AUDIO_INPUT_REFERENCE true`
- ES7210 + ES8311 duplex codec path
- input channel mask `MASK(0) + MASK(1)` when reference input is enabled
- ESP-SR AFE input format `MR` and device-side AEC enabled through
  `CONFIG_USE_DEVICE_AEC=y`

The local project matches that mapping. The qualification script exposes
`--box-ref-channel` only to compare candidate ES7210 reference channels during
standalone AEC testing. The normal firmware remains aligned with the upstream
mapping.

## Test Commands

Formal quiet-room run:

```bash
python3 scripts/aec_qualification/run.py \
  --board esp-box-3 \
  --auto-port \
  --box-ref-channel 1 \
  --skip-build \
  --no-capture-reset \
  --capture-timeout 240
```

Reference-channel comparison runs:

```bash
python3 scripts/aec_qualification/run.py --board esp-box-3 --auto-port --box-ref-channel 1
python3 scripts/aec_qualification/run.py --board esp-box-3 --auto-port --box-ref-channel 2
python3 scripts/aec_qualification/run.py --board esp-box-3 --auto-port --box-ref-channel 3
```

`--no-capture-reset` is useful on BOX-3 because USB Serial/JTAG can re-enumerate
after reset on macOS. If the host opens the old device node during that window,
the capture may miss early `silence` / `far_playback` events. A report that only
contains `barge_in` is invalid for admission.

## Formal Result

Quiet-room report:

`reports/aec_qualification/20260624-164633-esp-box-3-refch1/report.json`

Measured metrics:

| Case | ERLE | Raw | AEC Out | Reference | Metric Source |
| --- | ---: | ---: | ---: | ---: | --- |
| `silence` | 1.51 dB | -65.04 dBFS | -66.55 dBFS | -58.00 dBFS | `device_full_case_energy` |
| `far_playback` | 24.81 dB | -31.16 dBFS | -55.97 dBFS | -31.97 dBFS | `device_full_case_energy` |
| `barge_in` | 20.36 dB | -31.13 dBFS | -51.49 dBFS | -31.98 dBFS | `device_full_case_energy` |

Decision reasons:

- Far-playback ERLE 24.81 dB passes the `barge-in` threshold of 18 dB.
- Far-playback AEC residual -55.97 dBFS passes the threshold of `< -45 dBFS`.
- Clipping ratio passed.
- Barge-in ERLE 20.36 dB was recorded as a double-talk reference metric.

## Reference Channel Comparison

The reference-channel comparison was run to identify the correct ES7210 channel
ordering for BOX-3. It is not needed for routine admission once `refch1` is
validated.

| Channel | Decision | Far ERLE | Far AEC Residual | Reference Level | Barge ERLE | Report |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| `refch1` | `barge-in` | 22.93 dB | -54.18 dBFS | -31.97 dBFS | 19.38 dB | `reports/aec_qualification/20260624-162223-esp-box-3-refch1/report.json` |
| `refch2` | `PTT` | 13.76 dB | -46.11 dBFS | -68.66 dBFS | 15.20 dB | `reports/aec_qualification/20260624-161223-esp-box-3-refch2/report.json` |
| `refch3` | `fail` | 0.57 dB | -32.49 dBFS | -94.77 dBFS | 0.35 dB | `reports/aec_qualification/20260624-161649-esp-box-3-refch3/report.json` |

Interpretation:

- `refch1` has strong reference energy and passes both ERLE and residual echo
  gates.
- `refch2` has weak reference energy and falls below the `barge-in` ERLE gate.
- `refch3` is effectively not a useful reference channel in the current codec
  ordering.

## Environment Notes

Use a quiet room for formal admission. Background noise generally makes the
result more conservative by raising the AEC output floor and reducing measured
ERLE.

Recommended placement:

- Keep the BOX-3 at least 50 cm away from the host computer to reduce fan noise
  and table-reflection coupling.
- For host-played near speech, use a fixed external speaker when possible.
- Place the near-speech speaker 50-100 cm from the BOX-3, same height, with a
  fixed volume.
- Reuse the same position and volume for future device comparisons.

The invalid report
`reports/aec_qualification/20260624-164602-esp-box-3-refch1/report.json`
captured only `barge_in` and missed `silence` / `far_playback`; it must not be
used for admission.

## Admission Rule

For BOX-3 with this firmware and test method:

- `refch1` + quiet-room formal run passing `barge-in` thresholds means the board
  can be admitted in LiveKit `barge-in` mode.
- `refch2` and `refch3` are comparison-only paths and should not be used for the
  BOX-3 admission decision.
